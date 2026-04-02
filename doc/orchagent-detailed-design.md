# Thiết kế chi tiết OrchAgent và các Orch liên quan (Task 4.1)

## 1. Mục tiêu

Tài liệu này mô tả thiết kế chi tiết cho `orchagent`, bao gồm:

- Cấu trúc điều phối trung tâm của `orchdaemon`
- Khung xử lý chung của `Orch`/`Consumer`/`Executor`
- Thiết kế các orch lõi và phụ thuộc giữa chúng
- Luồng xử lý sự kiện từ DB tới SAI và phản ánh trạng thái
- Chiến lược retry, warm restart, quan sát và mở rộng

Phạm vi thiết kế tập trung vào các thành phần vận hành chính trong `orchagent/`.

## 2. Bức tranh tổng thể

### 2.1 Thành phần trung tâm

- `OrchDaemon`: tiến trình chính, khởi tạo DB connector, orch list, vòng lặp `Select`.
- `Orch`: base class cho orchestrator, quản lý tập `Consumer` theo từng bảng DB.
- `Consumer`: nhận dữ liệu từ bảng, gom vào `m_toSync`, gọi `drain()`.
- `Executor`: abstraction cho đối tượng được `Select` theo dõi và dispatch.

### 2.2 Chuỗi dữ liệu tiêu chuẩn

1. Cấu hình đi vào `CONFIG_DB`.
2. `cfgmgr/*mgrd` chuyển đổi vào `APPL_DB`.
3. `orchagent` đọc `APPL_DB`, thực thi logic orchestration.
4. Orch gọi API SAI để lập trình ASIC.
5. Trạng thái phản ánh qua `STATE_DB`/`COUNTERS_DB`, đối soát bằng `ASIC_DB`.

## 3. Thiết kế `orchdaemon`

### 3.1 Trách nhiệm

- Khởi tạo toàn bộ orch theo đúng thứ tự phụ thuộc.
- Đăng ký selectables từ các orch (consumer/notification/timer).
- Chạy event loop với heartbeat và flush pipeline.
- Điều phối retry phase, warm restart phase.

### 3.2 Vòng đời

1. `init()`:
- Tạo connector tới `APPL_DB`, `CONFIG_DB`, `STATE_DB`, `ASIC_DB`.
- Khởi tạo biến toàn cục cần thiết (`gSwitchOrch`, `gPortsOrch`, `gCrmOrch`, ...).
- Dựng orch list theo dependency order.

2. `start(heartbeatInterval)`:
- Đăng ký selectables vào `Select`.
- Vòng lặp `select()` lấy sự kiện.
- Gọi `Executor::execute()` tương ứng.
- Sau mỗi chu kỳ: xử lý retry, flush deferred updates, heartbeat.

3. `deinit()/exit`:
- Flush pending tasks.
- Giải phóng orch theo thứ tự ngược dependency.

### 3.3 Thứ tự khởi tạo đề xuất

1. `SwitchOrch` (năng lực và object switch toàn cục)
2. `PortsOrch` (port/VLAN/LAG là nền cho các orch khác)
3. `IntfsOrch`, `VrfOrch` (L3 interface và VRF)
4. `NeighOrch` (phụ thuộc port/intf)
5. `RouteOrch` (phụ thuộc neigh/nhg)
6. `FdbOrch`, `AclOrch`, `QosOrch`, `MirrorOrch`, `PolicerOrch`, ...
7. `CrmOrch`/telemetry orch theo dõi tài nguyên và observability

## 4. Thiết kế `Orch` base framework

### 4.1 Cấu trúc dữ liệu chính

- `m_consumerMap`: map table name -> `Consumer`.
- `Consumer::m_toSync`: map key -> `KeyOpFieldsValuesTuple` đang chờ xử lý.
- Retry cache (nếu bật): lưu task thất bại tạm thời theo constraint.

### 4.2 Mô hình xử lý

- `doTask()` không tham số: dispatch toàn bộ consumer có dữ liệu.
- `doTask(Consumer&)`: xử lý theo bảng cụ thể.
- `task_process_status` điều khiển hành vi sau xử lý:
- `task_success`: remove khỏi queue.
- `task_need_retry`: giữ/đẩy retry cache.
- `task_failed`: ghi log, giới hạn retry, tránh loop vô hạn.
- `task_invalid_entry`: loại bỏ entry xấu, tiếp tục luồng.

### 4.3 Quy tắc chuẩn cho orch mới

- Không block lâu trong `doTask`.
- Xử lý theo từng entry và luôn có nhánh xử lý lỗi rõ ràng.
- Mọi gọi SAI phải kiểm tra status và map về `task_process_status`.
- Bảo toàn idempotency khi nhận lại cùng key.

## 5. Thiết kế chi tiết các orch liên quan

### 5.1 `SwitchOrch`

Vai trò:
- Khởi tạo các thuộc tính switch toàn cục.
- Công bố capability cho orch khác.

Input chính:
- `APP_SWITCH_TABLE`/notifications liên quan switch.

Output:
- SAI switch attributes, trạng thái capability trong STATE DB.

### 5.2 `PortsOrch`

Vai trò:
- Quản lý vòng đời port vật lý, VLAN, LAG và mapping object ID.

Input chính:
- `APP_PORT_TABLE`, `APP_VLAN_TABLE`, `APP_LAG_TABLE`, `APP_LAG_MEMBER_TABLE`.

Output:
- SAI port/vlan/lag calls.
- Cập nhật operational status sang `STATE_PORT_TABLE`.

Phụ thuộc:
- Cần `SwitchOrch` sẵn sàng để lấy switch capability/object.

### 5.3 `IntfsOrch` và `VrfOrch`

Vai trò:
- Quản lý router interface và VRF object.

Input:
- `APP_INTF_TABLE`, `APP_VRF_TABLE`.

Output:
- SAI router interface/virtual router objects.

Phụ thuộc:
- `PortsOrch` (port/rif binding), `SwitchOrch`.

### 5.4 `NeighOrch`

Vai trò:
- Đồng bộ neighbor ARP/NDP vào SAI neighbor entry.

Input:
- `APP_NEIGH_TABLE`.

Output:
- SAI neighbor create/remove/set.

Phụ thuộc:
- Port/intf phải tồn tại trước khi tạo neighbor.

### 5.5 `RouteOrch`

Vai trò:
- Lập trình route IPv4/IPv6/MPLS và quản lý next-hop group.

Input:
- `APP_ROUTE_TABLE`, bảng NHG liên quan.

Output:
- SAI route entries, nhg object, trạng thái route phụ thuộc.

Phụ thuộc:
- `NeighOrch`, `IntfsOrch`, `VrfOrch`.

### 5.6 `FdbOrch`

Vai trò:
- Quản lý MAC forwarding entries và học MAC.

Input:
- `APP_FDB_TABLE`, notifications từ syncd.

Output:
- SAI FDB entry updates và state reflection.

Phụ thuộc:
- `PortsOrch` (bridge port/vlan).

### 5.7 `AclOrch`, `QosOrch`, `MirrorOrch`, `PolicerOrch`

Vai trò:
- Lập trình policy plane.

Input:
- Các bảng APP_DB policy tương ứng.

Output:
- SAI ACL/QoS/Mirror/Policer objects + binding với port/interface.

Phụ thuộc:
- `PortsOrch`, `SwitchOrch`, đôi khi `RouteOrch` theo tính năng.

## 6. Retry, lỗi và ổn định hệ thống

### 6.1 Retry theo constraint

- Khi thiếu dependency (ví dụ neighbor chưa có), entry chuyển `task_need_retry`.
- Retry cache gắn với constraint key để wakeup có mục tiêu khi dependency được giải.

### 6.2 Nguyên tắc xử lý lỗi SAI

- Mọi status trả về từ SAI phải qua hàm mapping chuẩn.
- Lỗi invalid input -> `task_invalid_entry`.
- Lỗi tạm thời -> `task_need_retry`.
- Lỗi nghiêm trọng -> `task_failed` + telemetry/log.

### 6.3 Idempotency và dedup

- Cùng key cập nhật nhiều lần phải merge hợp lý trong `m_toSync`.
- DEL mới có thể hủy pending SET cũ không còn giá trị.

## 7. Warm restart và nhất quán trạng thái

- Dùng trợ giúp warm restart để đọc map hiện hữu trước reconcile.
- Reconcile theo nguyên tắc “không phá trạng thái đúng đang tồn tại trên ASIC”.
- Mọi orch cần đảm bảo khả năng khôi phục từ DB snapshot.

## 8. Quan sát và telemetry

- Dữ liệu vận hành xuất ra `STATE_DB` (trạng thái dịch vụ/object).
- Counter thống kê xuất qua `COUNTERS_DB`.
- ResponsePublisher phản hồi trạng thái apply cho luồng quản trị.

## 9. Trình tự xử lý mẫu

### 9.1 Cấu hình route mới

1. App ghi route vào `CONFIG_DB`.
2. cfgmgr tạo entry ở `APP_ROUTE_TABLE`.
3. `RouteOrch::doTask()` nhận entry.
4. Kiểm tra dependency (vrf/intf/neigh/nhg).
5. Gọi SAI create route.
6. Cập nhật state và xóa entry khỏi queue.
7. Nếu dependency thiếu hoặc lỗi tạm thời, đẩy retry.

### 9.2 Port flap

1. syncd đẩy trạng thái port down/up vào DB state.
2. `PortsOrch` cập nhật cache trạng thái.
3. Orch liên quan (neigh/route/fdb) phản ứng theo dependency graph.
4. Hệ thống converges về trạng thái nhất quán.

## 10. Interface contract đề xuất giữa các orch

- Chỉ expose API tối thiểu, tránh coupling trực tiếp vào dữ liệu nội bộ.
- Các truy vấn phổ biến:
- tra cứu port/bridge port/router interface by alias.
- tra cứu vrf/nhg object id.
- kiểm tra readiness của dependency.

## 11. Hướng dẫn mở rộng orch mới

1. Định nghĩa bảng APP_DB input và schema field.
2. Tạo class kế thừa `Orch`, đăng ký consumer table.
3. Cài `doTask(Consumer&)` với mapping status rõ ràng.
4. Tích hợp init order trong `orchdaemon` theo dependency.
5. Bổ sung test mock:
- trạng thái thành công
- dependency thiếu/retry
- idempotency và merge updates
- warm restart reconcile

## 12. Rủi ro và biện pháp

- Rủi ro dead/retry loop: dùng retry budget + constraint-based wakeup.
- Rủi ro init order sai: khóa dependency trong pha init và assert rõ ràng.
- Rủi ro divergence DB/ASIC: định kỳ reconcile và telemetry mismatch.

## 13. Tiêu chí chấp nhận cho Task 4.1

- Có tài liệu thiết kế chi tiết cho `orchagent` và orch chính.
- Mô tả rõ dependency graph và init order.
- Mô tả rõ flow xử lý, retry, warm restart, observability.
- Có guideline mở rộng cho orch mới và chiến lược kiểm thử đi kèm.
