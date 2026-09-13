// Benchmark-only OTLP/gRPC byte sink. grpc-go still assembles each complete unary
// message, but the codec does NOT parse or validate the protobuf metric payload.
// Never use this receiver for production: it acknowledges and discards data.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"log"
	"net"
	"net/http"
	"sync/atomic"

	"google.golang.org/grpc"
)

type request struct{ size int }
type response struct{}
type rawCodec struct{}

func (rawCodec) Name() string { return "proto" }
func (rawCodec) Unmarshal(data []byte, value interface{}) error {
	value.(*request).size = len(data)
	return nil
}
func (rawCodec) Marshal(value interface{}) ([]byte, error) {
	_ = value.(*response)
	// An empty protobuf ExportMetricsServiceResponse means full success.
	return nil, nil
}

var requests, bytes atomic.Uint64
type service interface{}

func export(_ interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	r := new(request)
	if err := dec(r); err != nil { return nil, err }
	handler := func(context.Context, interface{}) (interface{}, error) {
		bytes.Add(uint64(r.size))
		requests.Add(1)
		return &response{}, nil
	}
	if interceptor != nil {
		return interceptor(ctx, r, &grpc.UnaryServerInfo{FullMethod: "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export"}, handler)
	}
	return handler(ctx, r)
}

func main() {
	endpoint := flag.String("listen", "127.0.0.1:24317", "benchmark gRPC address")
	stats := flag.String("stats", "127.0.0.1:28889", "HTTP request/byte counters")
	flag.Parse()
	listener, err := net.Listen("tcp", *endpoint)
	if err != nil { log.Fatal(err) }
	mux := http.NewServeMux()
	mux.HandleFunc("/stats", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]uint64{"requests": requests.Load(), "bytes": bytes.Load()})
	})
	go func() { log.Fatal(http.ListenAndServe(*stats, mux)) }()
	server := grpc.NewServer(grpc.ForceServerCodec(rawCodec{}), grpc.MaxRecvMsgSize(128*1024*1024))
	server.RegisterService(&grpc.ServiceDesc{
		ServiceName: "opentelemetry.proto.collector.metrics.v1.MetricsService",
		HandlerType: (*service)(nil),
		Methods: []grpc.MethodDesc{{MethodName: "Export", Handler: export}},
	}, struct{}{})
	log.Printf("DISCARDING all payloads without protobuf validation on %s", *endpoint)
	log.Fatal(server.Serve(listener))
}
