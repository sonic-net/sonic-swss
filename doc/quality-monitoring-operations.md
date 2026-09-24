# Chất lượng, giám sát và vận hành (Task 4.4)

## 1. Mục tiêu

Thiết lập khung vận hành cho SWSS theo ba trụ cột:

- Chất lượng (quality gates trước và trong khi apply)
- Giám sát (health, alert, SLO, telemetry)
- Vận hành (ops control, retry budget, maintenance mode)

## 2. Mô hình vận hành

### 2.1 Quality layer

- Pre-apply validation:
  - schema check
  - dependency check
  - policy check
- Runtime quality:
  - tỷ lệ lỗi SAI
  - độ sâu hàng đợi retry
  - độ trễ event loop

### 2.2 Monitoring layer

- STATE_DB:
  - health status theo dịch vụ/orch
  - alert hiện hành theo loại sự cố
- COUNTERS_DB:
  - SLO counters (lag, queue depth, processed events)
  - lỗi theo object family

### 2.3 Operations layer

- APP_DB control tables:
  - maintenance mode toggle
  - retry budget / backoff controls
  - enable/disable tính năng theo domain

## 3. Hợp đồng dữ liệu vận hành

### 3.1 QUALITY_GATE_TABLE (APP_DB)

Key:
- quality scope (ví dụ `pre-apply`, `runtime`)

Fields:
- `validation`: pass|fail
- `schema`: pass|fail
- `dependency`: pass|fail
- `reason`: mô tả nguyên nhân khi fail

### 3.2 SWSS_HEALTH_TABLE (STATE_DB)

Key:
- component/orch name (ví dụ `orchagent`, `p4orch`)

Fields:
- `status`: green|yellow|red
- `uptime_sec`: số không âm
- `last_transition`: unix timestamp

### 3.3 SWSS_ALERT_TABLE (STATE_DB)

Key:
- alert type (ví dụ `route_programming_latency`)

Fields:
- `severity`: warning|major|critical
- `threshold_*`: ngưỡng
- `observed_*`: giá trị quan sát
- `action`: mitigated|open|acked

### 3.4 SWSS_SLO_COUNTERS (COUNTERS_DB)

Key:
- scope (`global` hoặc theo module)

Fields (gợi ý):
- `event_loop_lag_ms`
- `retry_queue_depth`
- `processed_events`
- `sai_error_rate_ppm`

Contract:
- mọi counter là số không âm
- update theo snapshot hoặc tăng dần có kiểm soát theo từng field

### 3.5 SWSS_OPS_CONTROL_TABLE (APP_DB)

Key:
- domain (`orchagent`, `p4orch`, `retry_policy`, ...)

Fields:
- `maintenance`: enabled|disabled
- `max_retry`: số không âm
- `backoff_ms`: số không âm

## 4. SLO/SLA và ngưỡng đề xuất

- Event loop lag p99 < 50ms
- Retry queue depth ổn định quanh baseline, không tăng không giới hạn
- SAI error rate ppm dưới ngưỡng cảnh báo theo platform profile
- MTTD/MTTR cho alert quan trọng có mục tiêu rõ ràng theo môi trường

## 5. Luồng phản ứng vận hành

1. Monitoring phát hiện breach ngưỡng.
2. Alert được publish vào STATE_DB.
3. Ops controller cập nhật control table (ví dụ bật maintenance).
4. Orch giảm nhịp apply hoặc đổi retry profile.
5. Khi ổn định, clear alert và rollback control về baseline.

## 6. Quan hệ với các task trước

- Task 4.1: dùng dependency/init design để xác định health scope theo orch.
- Task 4.2: kế thừa contract key/field/reference cho bảng vận hành.
- Task 4.3: áp dụng cùng khung quality/monitoring/ops cho P4 path.

## 7. Kiểm thử cho Task 4.4

Bộ test bổ sung:
- tests/mock_tests/quality_monitoring_operations_ut.cpp

Phạm vi test:
- quality gates
- health/alert state
- slo counters
- ops control + maintenance/retry budget
- reset/resilience của toàn bộ view vận hành

## 8. Tiêu chí chấp nhận Task 4.4

- Có tài liệu chuẩn cho quality-monitoring-operations.
- Có contract dữ liệu vận hành rõ ràng theo DB role.
- Có test mock xác thực contract cốt lõi và resilience.
- Có liên kết vào kiến trúc tổng và tài liệu thiết kế/contract liên quan.
