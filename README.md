# SlurmTop

## Installation

### Requirements
- C++ compiler with C++11 support
- ncurses library
- SLURM cluster access

### Build
```bash
make
```

## Usage

```bash
./slurmtop <username>
```

### Controls

- **1-4**: Switch between views (Overview/Running/Pending/All)
- **↑/↓**: Scroll up/down through job list
- **←/→**: Focus column (expand to show full content)
- **PgUp/PgDn**: Scroll by page
- **R**: Refresh data
- **Q**: Quit

### Views

#### Overview
Summary statistics including:
- Total jobs (running/pending)
- GPU allocations by type (running)
- GPU requests by type (pending)

#### Running Jobs
Displays currently running jobs with:
- Job ID, Name, Account
- Runtime and Time Limit
- GPU count and type
- Status

#### Pending Jobs
Shows queued jobs with:
- Job ID, Name, Account
- Reason for pending
- Time Limit
- GPU request (count and type)
- Priority value
- Number of higher priority jobs in queue

#### All Jobs
Combined view of all jobs

### Column Focus

Press **←/→** to cycle through columns. The focused column will:
- Show `[Column Name]` in red brackets
- Expand to display full content without truncation
- Other columns shrink to accommodate

## Example

```bash
./slurmtop john_doe
```

## License

MIT License - feel free to use and modify as needed.
