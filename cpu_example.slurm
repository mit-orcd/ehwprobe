#!/bin/bash
#SBATCH --job-name=cpu_example
#SBATCH --partition=mit_quicktest
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --time=5:00
#SBATCH --output=cpu_example_%j.out

echo "Job $SLURM_JOB_ID running on $(hostname)"
echo "Allocated CPUs per task: $SLURM_CPUS_PER_TASK"
echo
$HOME/git/ehwprobe/ehwprobe --quiet
