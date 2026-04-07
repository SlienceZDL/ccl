#!/bin/bash
set -euo pipefail

NODE_RANK=${1:?need NODE_RANK}
PARALLEL_CFG=${2:-tp2_pp1_ep1}
SEQ_LEN=${3:-128}

GPUS_PER_NODE=${GPUS_PER_NODE:-8}
NUM_NODES=${NUM_NODES:-2}
MASTER_ADDR=${MASTER_ADDR:-33.255.169.92}
MASTER_PORT=${MASTER_PORT:-6000}
WORLD_SIZE=$((GPUS_PER_NODE * NUM_NODES))

ROOT_DIR=${ROOT_DIR:-/mnt/zdl/megatron_shared}
MEGATRON_DIR=${MEGATRON_DIR:-${ROOT_DIR}/code/Megatron-LM-msccl074}
MSCCL_DIR=${MSCCL_DIR:-${ROOT_DIR}/code/msccl}

CHECKPOINT_PATH=${CHECKPOINT_PATH:-${ROOT_DIR}/logs/qwen3_30b_run1}
TENSORBOARD_LOGS_PATH=${TENSORBOARD_LOGS_PATH:-${CHECKPOINT_PATH}}
VOCAB_FILE=${VOCAB_FILE:-${ROOT_DIR}/data/pretrain_MegatronLM/vocab.json}
MERGE_FILE=${MERGE_FILE:-${ROOT_DIR}/data/pretrain_MegatronLM/merges.txt}
DATA_PATH=${DATA_PATH:-${ROOT_DIR}/data/gpt_text_document_text_document}
MSCCL_XML_FILE=${MSCCL_XML_FILE:-${ROOT_DIR}/data/end-to-end/syccl/result-a100-ag-n16-0-Simple-fuse=False-depnop=False-i=4-inplace=True.xml}

PYTHON_BIN=${PYTHON_BIN:-python}
ENABLE_MSCCL=${ENABLE_MSCCL:-1}
LOAD_CHECKPOINT=${LOAD_CHECKPOINT:-1}
HOST_TAG=${HOSTNAME:-$(hostname)}

export CUDA_DEVICE_MAX_CONNECTIONS=1
export HF_DATASETS_OFFLINE=1
export TRANSFORMERS_OFFLINE=1
export HF_HUB_OFFLINE=1
export HF_HOME=${HF_HOME:-/mnt/zdl/.cache/huggingface}
export PYTHONIOENCODING=utf-8
export PYTHONUNBUFFERED=1
export LANG=${LANG:-C.UTF-8}
export LC_ALL=${LC_ALL:-C.UTF-8}
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-1}
export GLOO_SOCKET_IFNAME=${GLOO_SOCKET_IFNAME:-eth0}
export NCCL_SOCKET_IFNAME=${NCCL_SOCKET_IFNAME:-eth0}
export NCCL_DEBUG=${NCCL_DEBUG:-INFO}
export NCCL_DEBUG_SUBSYS=${NCCL_DEBUG_SUBSYS:-INIT,ENV,COLL}
export NCCL_ASYNC_ERROR_HANDLING=${NCCL_ASYNC_ERROR_HANDLING:-1}
export TORCH_NCCL_BLOCKING_WAIT=${TORCH_NCCL_BLOCKING_WAIT:-1}
export CUDA_VISIBLE_DEVICES=${CUDA_VISIBLE_DEVICES:-0,1,2,3,4,5,6,7}
export LD_LIBRARY_PATH="${MSCCL_DIR}/build/lib:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="${MEGATRON_DIR}:${PYTHONPATH:-}"
export TORCH_EXTENSIONS_DIR=${TORCH_EXTENSIONS_DIR:-/tmp/${USER:-zdl}/torch_extensions}
export MEGATRON_FUSED_KERNELS_BUILD_DIR=${MEGATRON_FUSED_KERNELS_BUILD_DIR:-/tmp/${USER:-zdl}/megatron_fused_kernels/${HOST_TAG%%.*}}

export ANTCCL_ENABLE=0
export NCCL_IB_DISABLE=${NCCL_IB_DISABLE:-0}
export NCCL_IB_GID_INDEX=${NCCL_IB_GID_INDEX:-0}
export NCCL_NET_GDR_LEVEL=${NCCL_NET_GDR_LEVEL:-0}
export NCCL_NET_GDR_READ=${NCCL_NET_GDR_READ:-0}
export NCCL_NVLS_ENABLE=${NCCL_NVLS_ENABLE:-0}

if [[ "${ENABLE_MSCCL}" == "1" ]]; then
  export NCCL_IB_HCA=${NCCL_IB_HCA:-mlx5_bond_0,mlx5_bond_1,mlx5_bond_2,mlx5_bond_3}
  export NCCL_PROTO=${NCCL_PROTO:-Simple}
  export NCCL_BUFFSIZE=${NCCL_BUFFSIZE:-2097152}
  export NCCL_ALGO=${NCCL_ALGO:-RING,TREE,MSCCL}
  export MSCCL_XML_FILES=${MSCCL_XML_FILES:-${MSCCL_XML_FILE}}
else
  unset MSCCL_XML_FILES 2>/dev/null || true
  export NCCL_ALGO=${NCCL_ALGO:-RING,TREE}
fi

case "${PARALLEL_CFG}" in
  tp2_pp1_ep1)
    TP=2
    PP=1
    ;;
  tp1_pp2_ep1)
    TP=1
    PP=2
    ;;
  tp1_pp1_ep1)
    TP=1
    PP=1
    ;;
  tp1_pp1_ep2)
    echo "[ERROR] expert parallel / MoE is not supported by Megatron-LM-msccl074." >&2
    echo "[ERROR] Use tp2_pp1_ep1 or tp1_pp2_ep1, or upgrade Megatron before running EP." >&2
    exit 2
    ;;
  *)
    echo "[ERROR] unknown PARALLEL_CFG=${PARALLEL_CFG}" >&2
    exit 1
    ;;
esac

if (( WORLD_SIZE % (TP * PP) != 0 )); then
  echo "[ERROR] WORLD_SIZE=${WORLD_SIZE} is not divisible by TP*PP=$((TP * PP))" >&2
  exit 1
fi

if (( NODE_RANK < 0 || NODE_RANK >= NUM_NODES )); then
  echo "[ERROR] NODE_RANK=${NODE_RANK} is outside [0, $((NUM_NODES - 1))]" >&2
  exit 1
fi

check_path() {
  local path=$1
  if [[ ! -e "${path}" ]]; then
    echo "[ERROR] missing path: ${path}" >&2
    exit 1
  fi
}

mkdir -p "${CHECKPOINT_PATH}" "${TENSORBOARD_LOGS_PATH}" \
  "${TORCH_EXTENSIONS_DIR}" "${MEGATRON_FUSED_KERNELS_BUILD_DIR}"

check_path "${MEGATRON_DIR}/pretrain_gpt.py"
check_path "${VOCAB_FILE}"
check_path "${MERGE_FILE}"
check_path "${DATA_PATH}.bin"
check_path "${DATA_PATH}.idx"

if [[ "${ENABLE_MSCCL}" == "1" ]]; then
  check_path "${MSCCL_XML_FILES}"
fi

if (( PP > 1 )) && (( 48 % PP != 0 )); then
  echo "[ERROR] num-layers=48 is not divisible by PP=${PP}" >&2
  exit 1
fi

DP=$((WORLD_SIZE / (TP * PP)))
echo "[INFO] WORLD_SIZE=${WORLD_SIZE}, TP=${TP}, PP=${PP}, DP=${DP}, SEQ_LEN=${SEQ_LEN}"
echo "[INFO] MASTER_ADDR=${MASTER_ADDR}, MASTER_PORT=${MASTER_PORT}, NODE_RANK=${NODE_RANK}"
echo "[INFO] fused kernel build dir: ${MEGATRON_FUSED_KERNELS_BUILD_DIR}"
echo "[INFO] MSCCL enabled: ${ENABLE_MSCCL}"
echo "[INFO] Megatron-LM-msccl074 does not support Qwen3 MoE/GQA/RoPE in this script."

DISTRIBUTED_ARGS=(
  --nproc_per_node "${GPUS_PER_NODE}"
  --nnodes "${NUM_NODES}"
  --node_rank "${NODE_RANK}"
  --master_addr "${MASTER_ADDR}"
  --master_port "${MASTER_PORT}"
  --use_env
)

MODEL_ARGS=(
  --num-layers 48
  --hidden-size 2048
  --ffn-hidden-size 6144
  --num-attention-heads 32
  --seq-length "${SEQ_LEN}"
  --max-position-embeddings 32768
  --make-vocab-size-divisible-by 128
  --layernorm-epsilon 1e-6
  --init-method-std 0.006
  --attention-dropout 0.0
  --hidden-dropout 0.0
  --openai-gelu
  --no-masked-softmax-fusion
  --no-bias-gelu-fusion
  --no-bias-dropout-fusion
)

MODEL_PARALLEL_ARGS=(
  --tensor-model-parallel-size "${TP}"
  --pipeline-model-parallel-size "${PP}"
)

TRAINING_ARGS=(
  --micro-batch-size 1
  --global-batch-size 32
  --train-iters 500000
  --lr 1.0e-4
  --min-lr 1.0e-5
  --lr-decay-style cosine
  --lr-warmup-fraction 0.001
  --lr-decay-iters 430000
  --weight-decay 0.1
  --adam-beta1 0.9
  --adam-beta2 0.95
  --clip-grad 1.0
  --bf16
  --distributed-backend nccl
  --DDP-impl local
  --num-workers 1
  --dataloader-type single
)

DATA_ARGS=(
  --data-path "${DATA_PATH}"
  --data-impl mmap
  --tokenizer-type GPT2BPETokenizer
  --vocab-file "${VOCAB_FILE}"
  --merge-file "${MERGE_FILE}"
  --split 949,50,1
)

EVAL_AND_LOGGING_ARGS=(
  --log-interval 10
  --save-interval 5000
  --eval-interval 1000
  --eval-iters 10
  --save "${CHECKPOINT_PATH}"
  --tensorboard-dir "${TENSORBOARD_LOGS_PATH}"
)

if [[ "${LOAD_CHECKPOINT}" == "1" ]] && [[ -f "${CHECKPOINT_PATH}/latest_checkpointed_iteration.txt" ]]; then
  EVAL_AND_LOGGING_ARGS+=(--load "${CHECKPOINT_PATH}")
fi

exec "${PYTHON_BIN}" -m torch.distributed.launch "${DISTRIBUTED_ARGS[@]}" \
  "${MEGATRON_DIR}/pretrain_gpt.py" \
  "${MODEL_ARGS[@]}" \
  "${MODEL_PARALLEL_ARGS[@]}" \
  "${TRAINING_ARGS[@]}" \
  "${DATA_ARGS[@]}" \
  "${EVAL_AND_LOGGING_ARGS[@]}"
