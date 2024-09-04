from ipaddress import ip_address as IP

def assert_sai_attribute_exists(attr_name, attrs, expected_val=None):
    assert attr_name in attrs, f"Attribute {attr_name} not found in {attrs}"
    if expected_val is not None:
        expected = expected_val
        actual = attrs[attr_name]
        # Attempt to convert to specific types to avoid string comparison when possible
        for type in [int, IP]:
            try:
                expected = type(expected)
                actual = type(actual)
                break
            except ValueError:
                continue
        assert actual == expected, f"Attribute {attr_name} value mismatch. Expected: {expected}, Actual: {actual}"
