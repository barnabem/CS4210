import pandas as pd
import matplotlib.pyplot as plt

OUTPUT_FILE = "/home/bmarty/cs4210/cs4210-Project1/graphs/Cummulative_output.png" 
INPUT_FILE = "/home/bmarty/cs4210/cs4210-Project1/data/Cummulative_output.csv"

# Load summarized dataset
df = pd.read_csv(INPUT_FILE)

# --- Extract useful parts from group_name ---
# Expected format: cXX_m_YY
df["credits"] = df["group_name"].str.extract(r"c(\d+)_")[0].astype(int)
df["matrix_size"] = df["group_name"].str.extract(r"_m_(\d+)")[0].astype(int)

# Sort for consistent plotting
df = df.sort_values(["matrix_size", "credits"])

# Get unique matrix sizes
matrix_sizes = sorted(df["matrix_size"].unique())

# Make 2x2 grid of subplots
fig, axes = plt.subplots(2, 2, figsize=(12, 10))
axes = axes.flatten()

for i, m in enumerate(matrix_sizes):
    ax = axes[i]
    subdf = df[df["matrix_size"] == m]

    # Plot stacked bar chart
    ax.bar(
        subdf["credits"].astype(str),
        subdf["mean_cpu_time"],
        label="CPU time",
        color="teal"
    )
    ax.bar(
        subdf["credits"].astype(str),
        subdf["mean_wait_time"],
        bottom=subdf["mean_cpu_time"],
        label="Wait time",
        color="salmon"
    )

    ax.set_title(f"Matrix size {m}x{m}")
    ax.set_xlabel("Credits")
    ax.set_ylabel("Mean execution time (us)")
    ax.legend()

plt.suptitle("Mean Execution Time by Credits (Stacked CPU + Wait Time)", fontsize=14)
plt.tight_layout(rect=[0, 0, 1, 0.96])

# ---- SAVE ----
plt.savefig(OUTPUT_FILE, dpi=300)
plt.close()

print(f"Figure saved at: {OUTPUT_FILE}")
