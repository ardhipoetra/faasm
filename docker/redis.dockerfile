# FROM redis:6-alpine
FROM registry.scontain.com/sconecuratedimages/apps:redis-6.2.6-bullseye

# Put config in place
COPY deploy/conf/redis-volatile.conf /redis.conf

CMD ["redis-server", "/redis.conf"]
