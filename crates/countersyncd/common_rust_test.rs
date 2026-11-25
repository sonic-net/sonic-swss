use swss_common::CxxString;

#[test]
fn common_strings_use_the_consumers_serde_traits() {
    let original = "counter\0field: \u{03bb}";
    let native = CxxString::new(original);
    let cloned = native.clone();
    drop(native);

    let encoded = serde_json::to_string(&cloned).unwrap();
    assert_eq!(encoded, serde_json::to_string(original).unwrap());

    let decoded: CxxString = serde_json::from_str(&encoded).unwrap();
    assert_eq!(decoded.as_bytes(), original.as_bytes());
    assert_eq!(decoded, cloned);
}
