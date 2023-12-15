FROM registry.scontain.com/ardhipoetra/registry/cloudskin:minio

CMD ["server", "/data/minio"]
