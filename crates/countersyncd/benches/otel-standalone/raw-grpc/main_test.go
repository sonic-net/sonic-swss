package main

import (
    "context"
    "testing"
)

func TestRawCodecCountsOpaquePayloadWithoutParsing(t *testing.T) {
    requests.Store(0)
    bytes.Store(0)
    payload := []byte{0xff, 0xff, 0xff} // deliberately not valid metric protobuf
    result, err := export(nil, context.Background(), func(v interface{}) error {
        return (rawCodec{}).Unmarshal(payload, v)
    }, nil)
    if err != nil { t.Fatal(err) }
    encoded, err := (rawCodec{}).Marshal(result)
    if err != nil || len(encoded) != 0 { t.Fatalf("response must be empty protobuf: %v %v", encoded, err) }
    if requests.Load() != 1 || bytes.Load() != uint64(len(payload)) {
        t.Fatal("complete request/byte counters mismatch")
    }
}
