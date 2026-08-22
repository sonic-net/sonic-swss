use std::{collections::HashSet, sync::Arc, time::Duration};

use criterion::{
    criterion_group, criterion_main, BatchSize, BenchmarkId, Criterion, SamplingMode, Throughput,
};

use countersyncd::{
    actor::aggregator::Aggregator,
    message::{
        aggregator::{AggregatedStatsMessage, AggregatorConfig, CounterSelector},
        saistats::{SAIStat, SAIStats, SAIStatsMessage},
    },
};

const SESSION_KEY: &str = "profile|PORT";
const SAMPLES_PER_ITERATION: usize = 1_000;
const SAMPLE_INTERVAL_NS: u64 = 1_000_000;
const REPORTING_RATE_US: u32 = 100_000;
const HEATMAP_INTERVAL_US: u32 = 1_000_000;
const HEATMAP_BUCKET_BOUNDARIES: [u64; 7] = [0, 64, 256, 1_024, 4_096, 16_384, 65_536];

#[derive(Clone, Copy)]
struct Scenario {
    name: &'static str,
    configured: bool,
    reporting_rate: bool,
    rollover: bool,
    heatmap: bool,
}

const SCENARIOS: [Scenario; 9] = [
    Scenario {
        name: "unconfigured_passthrough",
        configured: false,
        reporting_rate: false,
        rollover: false,
        heatmap: false,
    },
    Scenario {
        name: "configured_passthrough",
        configured: true,
        reporting_rate: false,
        rollover: false,
        heatmap: false,
    },
    Scenario {
        name: "reporting",
        configured: true,
        reporting_rate: true,
        rollover: false,
        heatmap: false,
    },
    Scenario {
        name: "rollover",
        configured: true,
        reporting_rate: false,
        rollover: true,
        heatmap: false,
    },
    Scenario {
        name: "heatmap",
        configured: true,
        reporting_rate: false,
        rollover: false,
        heatmap: true,
    },
    Scenario {
        name: "reporting_rollover",
        configured: true,
        reporting_rate: true,
        rollover: true,
        heatmap: false,
    },
    Scenario {
        name: "reporting_heatmap",
        configured: true,
        reporting_rate: true,
        rollover: false,
        heatmap: true,
    },
    Scenario {
        name: "rollover_heatmap",
        configured: true,
        reporting_rate: false,
        rollover: true,
        heatmap: true,
    },
    Scenario {
        name: "all_methods",
        configured: true,
        reporting_rate: true,
        rollover: true,
        heatmap: true,
    },
];

fn build_samples(object_count: usize) -> Arc<Vec<SAIStats>> {
    Arc::new(
        (0..SAMPLES_PER_ITERATION)
            .map(|sample_index| {
                let stats = (0..object_count)
                    .map(|object_index| SAIStat {
                        object_name: format!("Ethernet{}", object_index * 8),
                        type_id: 1,
                        stat_id: 1,
                        // Force a decrease every 250 samples so rollover scenarios
                        // exercise their correction path rather than only the steady state.
                        counter: raw_counter(sample_index, object_count, object_index),
                    })
                    .collect();
                SAIStats::new((sample_index as u64 + 1) * SAMPLE_INTERVAL_NS, stats)
            })
            .collect(),
    )
}

fn configured_aggregator(scenario: Scenario) -> Aggregator {
    let mut aggregator = Aggregator::default();
    if !scenario.configured {
        return aggregator;
    }

    let selector = CounterSelector::new(1, 1);
    aggregator.set_config(
        SESSION_KEY.to_string(),
        Some(AggregatorConfig {
            reporting_rate: scenario.reporting_rate.then_some(REPORTING_RATE_US),
            rollover_counters: scenario
                .rollover
                .then(|| HashSet::from([selector]))
                .unwrap_or_default(),
            heatmap_interval: scenario.heatmap.then_some(HEATMAP_INTERVAL_US),
            heatmap_counters: scenario
                .heatmap
                .then(|| HashSet::from([selector]))
                .unwrap_or_default(),
            heatmap_bucket_boundaries: scenario
                .heatmap
                .then(|| HEATMAP_BUCKET_BOUNDARIES.to_vec())
                .unwrap_or_default(),
        }),
    );
    aggregator
}

struct BenchmarkOutput {
    _aggregator: Aggregator,
    messages: Vec<AggregatedStatsMessage>,
}

fn expected_message_count(scenario: Scenario) -> usize {
    if scenario.reporting_rate {
        SAMPLES_PER_ITERATION / samples_per_reporting_window() + 1
    } else {
        SAMPLES_PER_ITERATION + 2
    }
}

fn samples_per_reporting_window() -> usize {
    REPORTING_RATE_US as usize * 1_000 / SAMPLE_INTERVAL_NS as usize
}

fn run_scenario(
    mut aggregator: Aggregator,
    samples: Vec<SAIStatsMessage>,
    closing_samples: [SAIStatsMessage; 2],
    mut messages: Vec<AggregatedStatsMessage>,
) -> BenchmarkOutput {
    for sample in samples {
        messages.extend(aggregator.process(Some(Arc::from(SESSION_KEY)), sample));
    }
    for sample in closing_samples {
        messages.extend(aggregator.process(Some(Arc::from(SESSION_KEY)), sample));
    }

    BenchmarkOutput {
        _aggregator: aggregator,
        messages,
    }
}

fn raw_counter(sample_index: usize, object_count: usize, object_index: usize) -> u64 {
    ((sample_index % 250) * object_count + object_index) as u64
}

fn expected_counters(object_count: usize, object_index: usize, rollover: bool) -> Vec<u64> {
    let mut last_raw = 0u64;
    let mut offset = 0u64;
    let mut corrected = 0u64;

    (0..SAMPLES_PER_ITERATION)
        .map(|sample_index| {
            let raw = raw_counter(sample_index, object_count, object_index);
            if rollover && sample_index > 0 && raw < last_raw {
                offset = corrected;
            }
            corrected = if rollover { offset + raw } else { raw };
            last_raw = raw;
            corrected
        })
        .collect()
}

fn expected_bucket_counts(values: impl IntoIterator<Item = u64>) -> Vec<u64> {
    let mut counts = vec![0; HEATMAP_BUCKET_BOUNDARIES.len() + 1];
    for value in values {
        let bucket = HEATMAP_BUCKET_BOUNDARIES
            .iter()
            .position(|bound| value <= *bound)
            .unwrap_or(HEATMAP_BUCKET_BOUNDARIES.len());
        counts[bucket] += 1;
    }
    counts
}

fn validate_scenario(scenario: Scenario, object_count: usize, base_samples: &[SAIStats]) {
    let samples = base_samples
        .iter()
        .cloned()
        .map(Arc::new)
        .collect::<Vec<SAIStatsMessage>>();
    let closing_samples = [
        Arc::new(SAIStats::new(
            u64::from(HEATMAP_INTERVAL_US) * 1_000 + 1,
            Vec::new(),
        )),
        Arc::new(SAIStats::new(
            u64::from(HEATMAP_INTERVAL_US) * 1_000 + u64::from(REPORTING_RATE_US) * 1_000 + 1,
            Vec::new(),
        )),
    ];
    let result = run_scenario(
        configured_aggregator(scenario),
        samples,
        closing_samples,
        Vec::with_capacity(expected_message_count(scenario)),
    );

    assert_eq!(result.messages.len(), expected_message_count(scenario));
    let expected_counters = (0..object_count)
        .map(|object_index| expected_counters(object_count, object_index, scenario.rollover))
        .collect::<Vec<_>>();
    let data_message = if scenario.reporting_rate {
        &result.messages[result.messages.len() - 2]
    } else {
        &result.messages[SAMPLES_PER_ITERATION - 1]
    };
    assert_eq!(data_message.stats.stats.len(), object_count);
    for (object_index, stat) in data_message.stats.stats.iter().enumerate() {
        assert_eq!(
            stat.counter,
            expected_counters[object_index][SAMPLES_PER_ITERATION - 1]
        );
    }

    let expected_heatmaps = if scenario.heatmap { object_count } else { 0 };
    assert_eq!(
        result
            .messages
            .iter()
            .map(|message| message.heatmaps.len())
            .sum::<usize>(),
        expected_heatmaps
    );

    if scenario.heatmap {
        let heatmap_message = result
            .messages
            .iter()
            .find(|message| !message.heatmaps.is_empty())
            .expect("completed heatmap window");
        if scenario.reporting_rate {
            let samples_per_reporting_window = samples_per_reporting_window();
            for heatmap in heatmap_message.heatmaps.iter() {
                let object_index = heatmap
                    .object_name
                    .strip_prefix("Ethernet")
                    .expect("benchmark object name")
                    .parse::<usize>()
                    .expect("benchmark object index")
                    / 8;
                let counters = &expected_counters[object_index];
                let reporting_values = counters
                    .iter()
                    .skip(samples_per_reporting_window - 1)
                    .step_by(samples_per_reporting_window)
                    .copied()
                    .collect::<Vec<_>>();
                assert_eq!(heatmap.count, reporting_values.len() as u64);
                assert_eq!(
                    heatmap.bucket_counts,
                    expected_bucket_counts(reporting_values)
                );
            }
        } else {
            for heatmap in heatmap_message.heatmaps.iter() {
                let object_index = heatmap
                    .object_name
                    .strip_prefix("Ethernet")
                    .expect("benchmark object name")
                    .parse::<usize>()
                    .expect("benchmark object index")
                    / 8;
                let counters = &expected_counters[object_index];
                assert_eq!(heatmap.count, SAMPLES_PER_ITERATION as u64);
                assert_eq!(
                    heatmap.bucket_counts,
                    expected_bucket_counts(counters.iter().copied())
                );
            }
        }
    } else {
        assert!(result
            .messages
            .iter()
            .all(|message| message.heatmaps.is_empty()));
    }
}

fn benchmark_aggregator(c: &mut Criterion) {
    let mut group = c.benchmark_group("aggregator_input_metrics_1ms_samples");
    group.sample_size(10);
    group.warm_up_time(Duration::from_secs(1));
    group.measurement_time(Duration::from_secs(5));
    group.sampling_mode(SamplingMode::Flat);

    for object_count in [1usize, 64] {
        let base_samples = build_samples(object_count);
        // Criterion reports these elements as input metrics/s. The two empty
        // samples used to close windows are timed but intentionally not counted.
        group.throughput(Throughput::Elements(
            (SAMPLES_PER_ITERATION * object_count) as u64,
        ));

        for scenario in SCENARIOS {
            validate_scenario(scenario, object_count, base_samples.as_slice());
            let base_samples = Arc::clone(&base_samples);
            group.bench_with_input(
                BenchmarkId::new(scenario.name, object_count),
                &scenario,
                move |bencher, scenario| {
                    bencher.iter_batched(
                        || {
                            let samples = base_samples
                                .iter()
                                .cloned()
                                .map(Arc::new)
                                .collect::<Vec<SAIStatsMessage>>();
                            let closing_samples = [
                                Arc::new(SAIStats::new(
                                    u64::from(HEATMAP_INTERVAL_US) * 1_000 + 1,
                                    Vec::new(),
                                )),
                                Arc::new(SAIStats::new(
                                    u64::from(HEATMAP_INTERVAL_US) * 1_000
                                        + u64::from(REPORTING_RATE_US) * 1_000
                                        + 1,
                                    Vec::new(),
                                )),
                            ];
                            (
                                configured_aggregator(*scenario),
                                samples,
                                closing_samples,
                                Vec::with_capacity(expected_message_count(*scenario)),
                            )
                        },
                        |(aggregator, samples, closing_samples, messages)| {
                            run_scenario(aggregator, samples, closing_samples, messages)
                        },
                        BatchSize::LargeInput,
                    );
                },
            );
        }
    }

    group.finish();
}

criterion_group!(benches, benchmark_aggregator);
criterion_main!(benches);
