import pandas as pd

FILEPATH = "/home/bmarty/cs4210/cs4210-Project1/data/Detailed_output_with_loadbalance.csv"

# Load the detailed results
df = pd.read_csv(FILEPATH)

# Group by group_name and calculate the mean
summary = df.groupby("group_name").agg({
    "cpu_time(us)": "mean",
    "wait_time(us)": "mean",
    "exec_time(us)": "mean"
}).reset_index()

# Rename columns to match the desired output format
summary.rename(columns={
    "cpu_time(us)": "mean_cpu_time",
    "wait_time(us)": "mean_wait_time",
    "exec_time(us)": "mean_exec_time"
}, inplace=True)

# Save the summary to a new CSV
summary.to_csv("/home/bmarty/cs4210/cs4210-Project1/data/Cummulative_output_with_loadbalance.csv", index=False)
