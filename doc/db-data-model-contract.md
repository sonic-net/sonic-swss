# Mô hình dữ liệu và hợp đồng cơ sở dữ liệu (Task 4.2)

## 1. Mục tiêu

Chuẩn hóa data model và database contract cho SWSS để:

- Giảm sai lệch giữa producer và consumer theo từng DB.
- Xác định rõ key format, field bắt buộc/tùy chọn, và ràng buộc tham chiếu.
- Tạo nền tảng kiểm thử tự động cho hợp đồng dữ liệu.

## 2. Phân lớp dữ liệu theo DB

1. CONFIG_DB
- Nguồn intent cấu hình từ quản trị.
- Không gọi SAI trực tiếp.
- Được cfgmgr chuyển đổi sang APPL_DB.

2. APPL_DB
- Hợp đồng vào của orchagent.
- Mỗi bảng ánh xạ một domain orchestration riêng.
- Mọi field không hợp lệ phải bị loại hoặc trả trạng thái lỗi.

3. STATE_DB
- Trạng thái vận hành thực tế đã hội tụ hoặc trạng thái trung gian có kiểm soát.
- Không dùng như nguồn intent cấu hình.

4. COUNTERS_DB
- Dữ liệu telemetry/counter quan sát được.
- Contract tập trung vào kiểu số không âm và key ổn định.

5. ASIC_DB
- Biểu diễn đối tượng SAI sau orchestration.
- Là lớp thực thi, không phải API cấu hình cho user.

## 3. Quy ước contract chung

### 3.1 Key format

- Mọi key phải có prefix bảng rõ ràng hoặc key payload có format được định nghĩa trước.
- Với bảng dạng đối tượng giao diện, key là alias chuẩn (ví dụ EthernetX, VlanY, PortChannelZ).
- Với bảng dạng quan hệ, key dùng phân tách rõ ràng bằng dấu ":".

### 3.2 Field contract

- Required fields: bắt buộc có mặt trước khi apply.
- Optional fields: nếu thiếu sẽ nhận default đã định nghĩa.
- Enum fields: chỉ nhận giá trị trong tập cho phép.
- Numeric fields: phải là số thập phân không âm, có thể thêm range theo bảng.

### 3.3 Reference contract

- Field tham chiếu phải trỏ tới key/object tồn tại trong domain nguồn.
- Nếu dependency chưa sẵn sàng: trả trạng thái retry thay vì failed ngay.
- DEL operation phải xóa hoặc vô hiệu hóa reference phụ thuộc theo policy bảng.

### 3.4 Update semantics

- Cùng key, SET nhiều lần sẽ merge field theo “last write wins” cho field trùng.
- DEL xóa toàn bộ entry và các derived state tương ứng (nếu có policy rõ).

## 4. Hợp đồng bảng trọng tâm (rút gọn cho orchestration lõi)

### 4.1 APP_PORT_TABLE

Key:
- interface name (ví dụ Ethernet0)

Required fields:
- admin_status: up|down

Optional fields:
- speed: số Mbps không âm
- mtu: số không âm

Contract:
- Key phải thuộc namespace port hợp lệ.
- admin_status là enum bắt buộc.

### 4.2 APP_VLAN_TABLE

Key:
- Vlan{vlan_id}

Required fields:
- admin_status: up|down

Optional fields:
- mtu

Contract:
- vlan_id phải là số.
- Prefix key bắt buộc là Vlan.

### 4.3 APP_NEIGH_TABLE

Key:
- {ifname}:{ip}

Required fields:
- neigh (MAC)

Optional fields:
- family

Contract:
- ifname phải tồn tại trong domain port/intf trước khi apply.
- MAC phải theo định dạng XX:XX:XX:XX:XX:XX.

### 4.4 APP_ROUTE_TABLE

Key:
- prefix CIDR

Required fields (một trong các nhóm):
- nexthop + ifname
- hoặc nexthop_group

Optional fields:
- blackhole, weight, segment

Contract:
- Không chấp nhận entry thiếu toàn bộ thông tin next hop.
- Reference nexthop_group phải tồn tại khi dùng.

### 4.5 APP_FDB_TABLE

Key:
- {vlan_id}:{mac}

Required fields:
- port
- type (static|dynamic)

Contract:
- VLAN ID phải là số.
- type là enum hợp lệ.

### 4.6 STATE_PORT_TABLE

Key:
- interface name

Required fields:
- admin_state
- oper_status

Optional fields:
- speed
- duplex

Contract:
- STATE phản ánh trạng thái vận hành đã xác nhận.
- Không ghi giá trị enum ngoài tập cho phép.

### 4.7 COUNTERS domain tables

Key:
- interface/object id ổn định theo domain

Contract:
- Counter fields là số không âm.
- Update có thể ghi đè giá trị mới (snapshot style).

## 5. Contract cho luồng giữa các lớp

1. CONFIG_DB -> APPL_DB
- cfgmgr chịu trách nhiệm chuẩn hóa key/field theo contract APPL_DB.

2. APPL_DB -> Orch
- Orch chỉ consume dữ liệu hợp lệ theo contract; dữ liệu sai phải bị reject/retry rõ ràng.

3. Orch -> STATE_DB/COUNTERS_DB
- Chỉ publish trạng thái nhất quán với kết quả apply và khả năng quan sát.

4. Orch -> ASIC_DB (qua SAI)
- Mapping field phải quyết định, không mơ hồ, và kiểm tra status đầy đủ.

## 6. Chính sách lỗi và tương thích

- Schema violation:
- invalid key/field/value => invalid entry.
- thiếu dependency runtime => need retry.

- Backward compatibility:
- Thêm optional field không phá vỡ consumer cũ.
- Thêm required field cần policy migrate/versioning.

- Versioning:
- Khuyến nghị gắn version theo table family cho thay đổi lớn.

## 7. Kiểm thử hợp đồng

Task 4.2 bổ sung bộ unit test hợp đồng tại:
- tests/mock_tests/db_contract_model_ut.cpp

Các nhóm kiểm thử chính:
- Key format và enum contract.
- Required fields.
- Reference/dependency contract.
- Numeric counter/state contract.
- Multi-DB role separation contract.

## 8. Tiêu chí chấp nhận Task 4.2

- Có tài liệu data model + DB contract rõ ràng.
- Có test tự động xác minh contract cốt lõi.
- Có liên kết tài liệu từ phần kiến trúc chung.
