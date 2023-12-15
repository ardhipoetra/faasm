#!/bin/bash

INI_FILE="faasm.ini"
ENV_FILE="$(pwd)/.env"
source $ENV_FILE

declare -A vars

function getip() {
    echo `docker inspect $(docker compose ps -q $1) --format='{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}'`
}

vars['backend']="compose"
vars['cluster_name']=$COMPOSE_PROJECT_NAME

# vars['working_dir']= 

vars['upload_ip']=$(getip "upload")
vars['upload_port']=$UPLOAD_DOCKER_PORT
vars['upload_port_in_docker']=$UPLOAD_HOST_PORT 
vars['upload_host_in_docker']="upload"

vars['planner_ip']=$(getip "planner") 
vars['planner_host_in_docker']="planner"

vars['minio_port']=$MINIO_DOCKER_PORT
vars['minio_port_in_docker']=$MINIO_HOST_PORT

vars['planner_port']=$PLANNER_DOCKER_PORT
vars['planner_port_in_docker']=$PLANNER_HOST_PORT


vars['worker_names']="faasm-dev-scone-worker-1"
vars['worker_ips']=$(getip "worker")

vars['mount_source']='False'

echo "[Faasm]" > $INI_FILE
for i in "${!vars[@]}"; do
  echo "$i = ${vars[$i]}" >> $INI_FILE
done

echo "Done generating $INI_FILE from $ENV_FILE"
