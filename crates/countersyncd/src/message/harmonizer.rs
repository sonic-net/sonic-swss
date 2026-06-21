use log::warn;

/// CounterSyncd-side subset of HIGH_FREQUENCY_TELEMETRY_HARMONIZER.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct HarmonizerConfig {
    /// Reporting interval in microseconds.
    pub reporting_rate: Option<u32>,
}

impl HarmonizerConfig {
    pub fn parse(serialized: &str) -> Option<Self> {
        let trimmed = serialized.trim();
        if trimmed.is_empty() {
            return None;
        }

        let reporting_rate = if let Ok(value) = trimmed.parse::<u32>() {
            Some(value)
        } else {
            parse_named_u32(trimmed, "reporting_rate")
        };

        match reporting_rate {
            Some(0) => {
                warn!("Ignoring harmonizer reporting_rate=0");
                None
            }
            Some(value) => Some(Self {
                reporting_rate: Some(value),
            }),
            None => None,
        }
    }
}

fn parse_named_u32(input: &str, name: &str) -> Option<u32> {
    let start = input.find(name)? + name.len();
    let value_start = input[start..].char_indices().find_map(|(offset, ch)| {
        if ch.is_ascii_digit() {
            Some(start + offset)
        } else if ch.is_ascii_whitespace()
            || matches!(ch, '=' | ':' | ',' | ';' | '{' | '}' | '"' | '\'')
        {
            None
        } else {
            Some(input.len())
        }
    })?;

    if value_start >= input.len() {
        return None;
    }

    let value_end = input[value_start..]
        .char_indices()
        .find_map(|(offset, ch)| (!ch.is_ascii_digit()).then_some(value_start + offset))
        .unwrap_or(input.len());

    input[value_start..value_end].parse::<u32>().ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_reporting_rate_from_supported_formats() {
        assert_eq!(
            HarmonizerConfig::parse("100").unwrap().reporting_rate,
            Some(100)
        );
        assert_eq!(
            HarmonizerConfig::parse("reporting_rate=200")
                .unwrap()
                .reporting_rate,
            Some(200)
        );
        assert_eq!(
            HarmonizerConfig::parse("{\"reporting_rate\":300}")
                .unwrap()
                .reporting_rate,
            Some(300)
        );
        assert_eq!(
            HarmonizerConfig::parse("rollover_counters=PORT|A;reporting_rate:400")
                .unwrap()
                .reporting_rate,
            Some(400)
        );
    }

    #[test]
    fn parse_ignores_missing_or_zero_reporting_rate() {
        assert_eq!(HarmonizerConfig::parse(""), None);
        assert_eq!(HarmonizerConfig::parse("rollover_counters=PORT|A"), None);
        assert_eq!(HarmonizerConfig::parse("reporting_rate=0"), None);
    }
}
