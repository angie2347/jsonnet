FROM alpine:3.18.3 AS builder

RUN apk -U add build-base

WORKDIR /opt

COPY . /opt/jsonnet

RUN cd jsonnet && \
    make

FROM alpine:3.18.3

RUN apk add --no-cache libstdc++ 

COPY --from=builder /opt/jsonnet/jsonnet /usr/local/bin
COPY --from=builder /opt/jsonnet/jsonnetfmt /usr/local/bin

ENTRYPOINT ["/usr/local/bin/jsonnet"]
