# Tích hợp P4-enabled ASIC và phần mở rộng (Task 4.3)

## 1. Mục tiêu

Thiết kế và chuẩn hóa tích hợp P4 trong SWSS để:

- Cho phép orchestration object theo pipeline P4 qua P4Orch.
- Tách rõ object manager cốt lõi và extension manager.
- Đảm bảo luồng phản hồi trạng thái thống nhất cho APP_P4RT_TABLE.

## 2. Kiến trúc tích hợp

### 2.1 Thành phần chính

- P4Orch: dispatcher trung tâm cho bản ghi trong APP_P4RT_TABLE.
- P4OidMapper: cache ánh xạ P4 key -> SAI object OID/ref-count.
- TablesDefnManager: đọc và áp dụng table definition động cho pipeline.
- Các manager cốt lõi: RouterInterface, Neighbor, NextHop, Route.
- Các manager mở rộng: AclTable, AclRule, Wcmp, TunnelDecapGroup, ExtTables.

### 2.2 Điểm nối tại orchdaemon

- orchdaemon khởi tạo ZMQ server riêng cho P4Orch.
- P4Orch được thêm vào m_orchList để tham gia vòng select/event loop.
- P4Orch chỉ xử lý khi port readiness đạt yêu cầu.

## 3. Hợp đồng DB cho P4RT

### 3.1 Bảng đầu vào chính

- APP_P4RT_TABLE (APPL_DB)

Key contract:
- key dạng ghép: {table_name}:{serialized_match_key_or_json}
- table_name phải thuộc tập manager đã đăng ký trong P4Orch.

Operation contract:
- SET: enqueue vào manager tương ứng, xử lý theo precedence add-order.
- DEL: xử lý theo precedence del-order (reverse của add-order).

Response contract:
- Kết quả phải publish qua ResponsePublisher vào APP_P4RT_TABLE.
- Trạng thái invalid key/table phải trả INVALID_PARAM.

### 3.2 Bảng định nghĩa pipeline

- APP_P4RT_TABLES_DEFINITION_TABLE

Contract:
- payload JSON phải chứa danh sách tables.
- mỗi table cần id, alias, match fields, action spec hợp lệ.
- khi parse lỗi: trả INVALID_PARAM và không apply một phần mơ hồ.

### 3.3 Extension manager contract

- APP_P4RT_EXT_TABLES_MANAGER (logical manager name)

Contract:
- Bảng không thuộc manager cốt lõi sẽ được route vào extension manager.
- Extension manager chịu trách nhiệm xác thực schema riêng và publish status.
- Không được phá vỡ contract phản hồi thống nhất của APP_P4RT_TABLE.

## 4. Dependency và thứ tự xử lý

### 4.1 Add precedence

Thứ tự tham chiếu điển hình:
1. table definition
2. router interface
3. neighbor
4. tunnel/next-hop/wcmp
5. route
6. ACL/mirror/l3 admit/tunnel decap/extension

### 4.2 Delete precedence

- Delete theo thứ tự ngược lại add precedence để tránh vi phạm reference.

## 5. Hành vi lỗi, retry, ổn định

- Sai định dạng key/table: invalid entry.
- Thiếu dependency runtime: need retry nếu có thể hội tụ.
- Lỗi không thể phục hồi: failed + publish status.
- Warmboot: ưu tiên flush batch đồng nhất và giữ tính idempotent.

## 6. Quan sát và telemetry

- P4 counters được cập nhật qua timer executors (ACL/EXT counters) vào COUNTERS_DB.
- P4 operation status được phản ánh qua response publish path.

## 7. Phần mở rộng đề xuất

1. Thêm manager mới:
- đăng ký vào map table->manager.
- khai báo add/del precedence.
- triển khai deserialize/validate/apply/remove.

2. Thêm bảng mới:
- đảm bảo key contract tương thích mô hình table_name:key_payload.
- cập nhật test hợp đồng cho key/field/status.

3. Tương thích ngược:
- field mới nên là optional nếu không bắt buộc migration.
- giữ nguyên cơ chế publish status để client không vỡ giao thức.

## 8. Kiểm thử cho Task 4.3

Bổ sung test hợp đồng tại:
- tests/mock_tests/p4_enabled_asic_ext_ut.cpp

Nhóm kiểm thử:
- Key contract APP_P4RT_TABLE.
- Routing về core manager/extension manager.
- Definition payload contract.
- OID mapping logic contract (set/get/ref-count semantics ở mức khái niệm).
- Response/status contract cho lỗi input.

## 9. Tiêu chí chấp nhận Task 4.3

- Có tài liệu thiết kế tích hợp P4-enabled ASIC rõ ràng.
- Có mô tả contract cho core và extension path.
- Có unit test mock xác nhận contract chính.
- Có liên kết tài liệu từ architecture/design/contract docs hiện hữu.
