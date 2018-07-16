# DOCKER
.PHONY: build-base
build-base:
	docker build -t shillaker/faasm-base .

.PHONY: build-worker
build-worker:
	docker build -t shillaker/faasm-worker -f worker.dockerfile .

.PHONY: build-edge
build-edge:
	docker build -t shillaker/faasm-edge -f edge.dockerfile .

# DOCKER COMPOSE
start-all:
	docker-compose up -d

stop-all:
	docker-compose stop

restart-all:
	docker-compose stop; docker-compose up -d

restart-worker:
	docker-compose stop worker; docker-compose up -d worker

restart-edge:
	docker-compose stop edge; docker-compose up -d edge

bash-worker:
	docker-compose run worker bash -l

bash-edge:
	docker-compose run edge bash -l

# KUBERNETES
start-local:
	minikube start --vm-driver kvm2

deploy:
	kubectl apply -f k8s

# REDIS

redis:
	docker-compose run redis redis-cli -h redis