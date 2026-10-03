VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
LDFLAGS  = -s -w -X main.version=$(VERSION)

.PHONY: build test integration run minio clean

build:
	CGO_ENABLED=0 go build -ldflags '$(LDFLAGS)' -o dbox .

test:
	go vet ./...
	go test -race ./...

# Needs `make minio` first.
integration:
	go test -race -tags integration ./...

minio:
	docker compose up -d --wait
	for s in minio-a minio-b; do \
		docker compose exec $$s sh -c 'mc alias set local http://localhost:9000 minioadmin minioadmin >/dev/null && mc mb -p local/dbox'; \
	done

run: build
	mkdir -p tmp/box
	./dbox run --config dev.config.yaml

clean:
	rm -rf dbox dist tmp
