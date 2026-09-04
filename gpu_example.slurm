#!/bin/bash
#SBATCH --job-name=gpu_example
#SBATCH --partition=mit_normal_gpu
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --gres=gpu:1
#SBATCH --time=10:00
#SBATCH --output=gpu_example_%j.out

echo "Job $SLURM_JOB_ID running on $(hostname)"
echo "CUDA_VISIBLE_DEVICES=$CUDA_VISIBLE_DEVICES"
echo
nvidia-smi -L
echo
# full (non-quiet) run: includes the GPU/IB/NUMA locality report
$HOME/git/ehwprobe/ehwprobe
