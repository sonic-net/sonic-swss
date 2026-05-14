use log::LevelFilter::Debug;
use std::io::Write;
use std::sync::{Arc, Mutex, Once, OnceLock};

static INIT_ENV_LOGGER: Once = Once::new();

static LOG_BUFFER: OnceLock<Arc<Mutex<Vec<u8>>>> = OnceLock::new();

fn get_log_buffer() -> &'static Arc<Mutex<Vec<u8>>> {
    LOG_BUFFER.get_or_init(|| Arc::new(Mutex::new(Vec::new())))
}

pub fn capture_logs() -> String {
    INIT_ENV_LOGGER.call_once(|| {
        env_logger::builder()
            .is_test(true)
            .filter_level(Debug)
            .format({
                let buffer = get_log_buffer().clone();
                move |_, record| {
                    let mut buffer = buffer.lock().unwrap();
                    writeln!(buffer, "[{}] {}", record.level(), record.args()).unwrap();
                    Ok(())
                }
            })
            .init();
    });

    let buffer = get_log_buffer().lock().unwrap();
    String::from_utf8(buffer.clone()).expect("Log buffer should be valid UTF-8")
}

pub fn clear_logs() {
    let mut buffer = get_log_buffer().lock().unwrap();
    buffer.clear();
}

pub fn assert_logs(expected: Vec<&str>) {
    let logs_string = capture_logs();
    let mut logs = logs_string.lines().collect::<Vec<_>>();
    let mut reverse_expected = expected.clone();
    reverse_expected.reverse();
    logs.reverse();

    let mut match_count = 0;
    for line in logs {
        if reverse_expected.is_empty() {
            break;
        }
        if line.contains(reverse_expected[match_count]) {
            match_count += 1;
        }

        if match_count == reverse_expected.len() {
            break;
        }
    }
    assert_eq!(
        match_count,
        expected.len(),
        "\nexpected logs \n{}\n, got logs \n{}\n",
        expected.join("\n"),
        logs_string
    );
}
