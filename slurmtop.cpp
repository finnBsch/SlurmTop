#include <ncurses.h>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cstdio>
#include <algorithm>
#include <iomanip>
#include <memory>
#include <cstring>

// Job state enum
enum class JobState {
    RUNNING,
    PENDING,
    OTHER
};

// Job information structure
struct Job {
    std::string jobId;
    std::string jobName;
    std::string account;
    std::string state;
    std::string reason;
    int gpuCount;
    std::string gpuType;
    std::string runtime;
    std::string timeLimit;
    long priority;

    JobState getState() const {
        if (state == "RUNNING") return JobState::RUNNING;
        if (state == "PENDING") return JobState::PENDING;
        return JobState::OTHER;
    }
};

// Global data structure
struct SlurmData {
    std::string username;
    std::vector<Job> jobs;
    std::vector<Job> allPendingJobs; // All pending jobs in queue for priority comparison
    int totalJobs;
    int runningJobs;
    int pendingJobs;
    std::map<std::string, int> gpuTypeCount; // GPU type -> count for running jobs
    std::map<std::string, int> gpuTypeRequested; // GPU type -> count for pending jobs

    void clear() {
        jobs.clear();
        allPendingJobs.clear();
        gpuTypeCount.clear();
        gpuTypeRequested.clear();
        totalJobs = runningJobs = pendingJobs = 0;
    }
};

// Execute command and return output
std::string execCommand(const std::string& cmd) {
    std::string result;
    std::shared_ptr<FILE> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    return result;
}

// Strip control characters (newlines, tabs, etc) from string
std::string stripControlChars(const std::string& str) {
    std::string result;
    for (char c : str) {
        if (c >= 32 && c <= 126) {  // Only printable ASCII
            result += c;
        } else if (c == '\t') {
            result += ' ';  // Replace tabs with spaces
        }
        // Skip all other control characters including \n, \r
    }
    return result;
}

// Extract value from scontrol output
std::string extractField(const std::string& output, const std::string& fieldName) {
    size_t pos = output.find(fieldName + "=");
    if (pos == std::string::npos) return "";

    pos += fieldName.length() + 1;
    size_t end = output.find(' ', pos);
    if (end == std::string::npos) end = output.find('\n', pos);
    if (end == std::string::npos) end = output.length();

    std::string value = output.substr(pos, end - pos);
    return stripControlChars(value);  // Clean the extracted value
}

// Extract GPU info from AllocTRES or ReqTRES
void extractGPUInfo(const std::string& output, const std::string& fieldName, int& gpuCount, std::string& gpuType) {
    gpuCount = 0;
    gpuType = "N/A";

    // Find the field (AllocTRES= or ReqTRES=)
    size_t fieldPos = output.find(fieldName + "=");
    if (fieldPos == std::string::npos) return;

    // FIRST: Try to find typed GPU pattern (gres/gpu:TYPE=COUNT)
    // This is more specific and should be checked first
    size_t searchStart = fieldPos;
    size_t typedGresPos = output.find("gres/gpu:", searchStart);

    if (typedGresPos != std::string::npos) {
        // Pattern: gres/gpu:TYPE=COUNT
        size_t typeStart = typedGresPos + 9; // Length of "gres/gpu:"
        size_t typeEnd = output.find('=', typeStart);

        if (typeEnd != std::string::npos) {
            // Extract type
            gpuType = output.substr(typeStart, typeEnd - typeStart);
            gpuType = stripControlChars(gpuType);

            // Extract count
            size_t countStart = typeEnd + 1;
            size_t countEnd = output.find_first_of(" ,\n", countStart);
            if (countEnd == std::string::npos) countEnd = output.length();

            std::string countStr = output.substr(countStart, countEnd - countStart);
            try {
                gpuCount = std::stoi(countStr);
            } catch (...) {
                gpuCount = 0;
            }
            return; // Found typed GPU, we're done
        }
    }

    // SECOND: If no typed pattern found, try untyped pattern (gres/gpu=COUNT)
    size_t untypedGresPos = output.find("gres/gpu=", searchStart);
    if (untypedGresPos != std::string::npos) {
        // Pattern: gres/gpu=COUNT (no type specified)
        size_t countStart = untypedGresPos + 9; // Length of "gres/gpu="
        size_t countEnd = output.find_first_of(" ,\n", countStart);
        if (countEnd == std::string::npos) countEnd = output.length();

        std::string countStr = output.substr(countStart, countEnd - countStart);
        try {
            gpuCount = std::stoi(countStr);
        } catch (...) {
            gpuCount = 0;
        }
        gpuType = "generic";
    }
}

// Parse job details from scontrol output
Job parseJobDetails(const std::string& jobId, const std::string& scontrolOutput) {
    Job job;
    job.jobId = jobId;
    job.jobName = extractField(scontrolOutput, "JobName");
    job.account = extractField(scontrolOutput, "Account");
    job.state = extractField(scontrolOutput, "JobState");
    job.reason = extractField(scontrolOutput, "Reason");
    job.runtime = extractField(scontrolOutput, "RunTime");
    job.timeLimit = extractField(scontrolOutput, "TimeLimit");

    std::string priorityStr = extractField(scontrolOutput, "Priority");
    try {
        job.priority = std::stol(priorityStr);
    } catch (...) {
        job.priority = 0;
    }

    // Extract GPU info based on job state
    if (job.state == "RUNNING") {
        extractGPUInfo(scontrolOutput, "AllocTRES", job.gpuCount, job.gpuType);
    } else {
        // For pending jobs, try ReqTRES first, then AllocTRES
        extractGPUInfo(scontrolOutput, "ReqTRES", job.gpuCount, job.gpuType);
        if (job.gpuCount == 0) {
            extractGPUInfo(scontrolOutput, "AllocTRES", job.gpuCount, job.gpuType);
        }
    }

    return job;
}

// Fetch all SLURM data
void fetchSlurmData(SlurmData& data) {
    data.clear();

    // Get all job IDs for the user
    std::string cmd = "squeue -u " + data.username + " -h -o \"%i\"";
    std::string jobIdsOutput = execCommand(cmd);

    std::istringstream iss(jobIdsOutput);
    std::string jobId;
    std::vector<std::string> jobIds;

    while (iss >> jobId) {
        jobIds.push_back(jobId);
    }

    // Fetch details for each job
    for (const auto& jid : jobIds) {
        std::string scontrolCmd = "scontrol show job " + jid + " 2>/dev/null";
        std::string output = execCommand(scontrolCmd);

        if (!output.empty()) {
            Job job = parseJobDetails(jid, output);
            data.jobs.push_back(job);

            if (job.state == "RUNNING") {
                data.runningJobs++;
                if (job.gpuCount > 0) {
                    data.gpuTypeCount[job.gpuType] += job.gpuCount;
                }
            } else if (job.state == "PENDING") {
                data.pendingJobs++;
                if (job.gpuCount > 0) {
                    data.gpuTypeRequested[job.gpuType] += job.gpuCount;
                }
            }
        }
    }

    data.totalJobs = data.jobs.size();

    // Fetch all pending jobs for priority comparison
    std::string allPendingCmd = "squeue -h -t PD -o \"%i\"";
    std::string allPendingOutput = execCommand(allPendingCmd);

    std::istringstream pendingIss(allPendingOutput);
    while (pendingIss >> jobId) {
        std::string scontrolCmd = "scontrol show job " + jobId + " 2>/dev/null";
        std::string output = execCommand(scontrolCmd);

        if (!output.empty()) {
            Job job = parseJobDetails(jobId, output);
            if (job.priority > 0) {
                data.allPendingJobs.push_back(job);
            }
        }
    }

    // Sort all pending jobs by priority (descending)
    std::sort(data.allPendingJobs.begin(), data.allPendingJobs.end(),
              [](const Job& a, const Job& b) { return a.priority > b.priority; });
}

// UI class
class SlurmTopUI {
private:
    enum View {
        OVERVIEW = 0,
        RUNNING = 1,
        PENDING = 2,
        ALL = 3
    };

    View currentView;
    int scrollOffset;
    int maxRows;
    SlurmData& data;
    bool running;
    int focusedColumn;  // -1 for none, 0+ for column index

public:
    SlurmTopUI(SlurmData& d) : data(d), currentView(OVERVIEW), scrollOffset(0), running(true), focusedColumn(-1) {
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);

        if (has_colors()) {
            start_color();
            init_pair(1, COLOR_BLACK, COLOR_CYAN);    // Header
            init_pair(2, COLOR_CYAN, COLOR_BLACK);    // Highlighted text
            init_pair(3, COLOR_GREEN, COLOR_BLACK);   // Running jobs
            init_pair(4, COLOR_YELLOW, COLOR_BLACK);  // Pending jobs
            init_pair(5, COLOR_WHITE, COLOR_BLACK);   // Normal text
            init_pair(6, COLOR_RED, COLOR_BLACK);     // Important numbers
        }

        timeout(100); // Non-blocking input with 100ms timeout
    }

    ~SlurmTopUI() {
        endwin();
    }

    // Structure to hold column widths
    struct ColumnWidths {
        int jobId, jobName, account, col4, col5, col6, col7, col8, col9;
    };

    // Calculate max needed width for a specific column in the current data
    int getMaxColumnWidth(int columnIndex, const std::vector<Job>& jobs, bool isPendingView) {
        // Start with header width
        const char* pendingHeaders[9] = {"JobID", "JobName", "Account", "Reason", "TimeLimit", "GPUs", "GPU Type", "Priority", "Higher"};
        const char* runningHeaders[8] = {"JobID", "JobName", "Account", "Runtime", "TimeLimit", "GPUs", "GPU Type", "Status"};
        const char* header = isPendingView ? pendingHeaders[columnIndex] : runningHeaders[columnIndex];

        int maxWidth = strlen(header); // Start with header width

        for (const auto& job : jobs) {
            int len = 0;
            if (isPendingView) {
                // Pending view has 9 columns
                switch (columnIndex) {
                    case 0: len = job.jobId.length(); break;
                    case 1: len = job.jobName.length(); break;
                    case 2: len = job.account.length(); break;
                    case 3: len = job.reason.length(); break;
                    case 4: len = job.timeLimit.length(); break;
                    case 5: len = std::to_string(job.gpuCount).length(); break;
                    case 6: len = job.gpuType.length(); break;
                    case 7: len = std::to_string(job.priority).length(); break;
                    case 8:
                        // For "Higher" column, count jobs with higher priority
                        int higherCount = 0;
                        for (const auto& other : data.allPendingJobs) {
                            if (other.priority > job.priority) higherCount++;
                        }
                        len = std::to_string(higherCount).length();
                        break;
                }
            } else {
                // Running view has 8 columns
                switch (columnIndex) {
                    case 0: len = job.jobId.length(); break;
                    case 1: len = job.jobName.length(); break;
                    case 2: len = job.account.length(); break;
                    case 3: len = job.runtime.length(); break;
                    case 4: len = job.timeLimit.length(); break;
                    case 5: len = std::to_string(job.gpuCount).length(); break;
                    case 6: len = job.gpuType.length(); break;
                    case 7: len = job.state.length(); break;
                }
            }
            maxWidth = std::max(maxWidth, len);
        }

        return std::min(maxWidth + 1, 50); // +1 for spacing, cap at 50 chars max
    }

    // Calculate dynamic column widths based on focused column
    ColumnWidths calculateColumnWidths(int terminalCols, int numColumns, const int* defaultWidths,
                                      const std::vector<Job>& jobs, bool isPendingView) {
        ColumnWidths widths;
        int* widthArray[9] = {&widths.jobId, &widths.jobName, &widths.account,
                              &widths.col4, &widths.col5, &widths.col6,
                              &widths.col7, &widths.col8, &widths.col9};

        if (focusedColumn >= 0 && focusedColumn < numColumns) {
            // FOCUSED MODE: Expand focused column, distribute remaining to others
            int availableWidth = terminalCols - (numColumns - 1) - 2; // Subtract separators and margin

            // Calculate needed width for focused column (including header and brackets)
            int focusedNeededWidth = getMaxColumnWidth(focusedColumn, jobs, isPendingView);
            focusedNeededWidth += 2; // Add space for brackets [ ]

            // Focused column gets: min(needed_width, full_width)
            int focusedWidth = std::min(focusedNeededWidth, availableWidth);
            *widthArray[focusedColumn] = focusedWidth;

            // Remaining width for other columns
            int remainingWidth = availableWidth - focusedWidth;
            int numOtherColumns = numColumns - 1;
            int widthPerOtherColumn = remainingWidth / numOtherColumns;

            // Each other column gets: min(required_width, proportional_share)
            for (int i = 0; i < numColumns; i++) {
                if (i != focusedColumn) {
                    int requiredWidth = getMaxColumnWidth(i, jobs, isPendingView);
                    *widthArray[i] = std::min(requiredWidth, widthPerOtherColumn);
                }
            }

            // Distribute any leftover space to other columns that need it
            int usedByOthers = 0;
            for (int i = 0; i < numColumns; i++) {
                if (i != focusedColumn) {
                    usedByOthers += *widthArray[i];
                }
            }
            int leftover = remainingWidth - usedByOthers;

            if (leftover > 0) {
                // Give leftover to columns that were capped
                for (int i = 0; i < numColumns && leftover > 0; i++) {
                    if (i != focusedColumn) {
                        int requiredWidth = getMaxColumnWidth(i, jobs, isPendingView);
                        int canGrow = requiredWidth - *widthArray[i];
                        if (canGrow > 0) {
                            int toAdd = std::min(canGrow, leftover);
                            *widthArray[i] += toAdd;
                            leftover -= toAdd;
                        }
                    }
                }
                // Distribute any remaining evenly
                for (int i = 0; i < numColumns && leftover > 0; i++) {
                    if (i != focusedColumn) {
                        (*widthArray[i])++;
                        leftover--;
                    }
                }
            }
        } else {
            // DEFAULT MODE: Distribute width to fill terminal
            int availableWidth = terminalCols - (numColumns - 1) - 2; // Subtract separators and margin

            // Calculate required width for each column
            int totalRequired = 0;
            int requiredWidths[9];  // Changed from 8 to 9 to support pending view
            for (int i = 0; i < numColumns; i++) {
                requiredWidths[i] = getMaxColumnWidth(i, jobs, isPendingView);
                totalRequired += requiredWidths[i];
            }

            if (totalRequired <= availableWidth) {
                // All columns fit with room to spare - distribute extra space proportionally
                int extraSpace = availableWidth - totalRequired;

                // First, give each column its required width
                for (int i = 0; i < numColumns; i++) {
                    *widthArray[i] = requiredWidths[i];
                }

                // Then distribute extra space proportionally based on required widths
                for (int i = 0; i < numColumns && extraSpace > 0; i++) {
                    int proportionalExtra = (requiredWidths[i] * extraSpace) / totalRequired;
                    // Cap extra growth to avoid excessive widths
                    int maxGrowth = std::min(proportionalExtra, 20);
                    *widthArray[i] += maxGrowth;
                    extraSpace -= maxGrowth;
                }

                // Distribute any remaining space evenly
                for (int i = 0; i < numColumns && extraSpace > 0; i++) {
                    (*widthArray[i])++;
                    extraSpace--;
                }
            } else {
                // Columns need more space than available - shrink proportionally
                for (int i = 0; i < numColumns; i++) {
                    *widthArray[i] = (requiredWidths[i] * availableWidth) / totalRequired;
                    // Ensure minimum width
                    int minWidth = (i == 4 || i == 5) ? 5 : 8;
                    *widthArray[i] = std::max(*widthArray[i], minWidth);
                }
            }
        }

        return widths;
    }

    void drawHeader() {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);

        attron(COLOR_PAIR(1) | A_BOLD);
        mvhline(0, 0, ' ', cols);
        mvprintw(0, 2, "SLURM Top - User: %s", data.username.c_str());

        // View indicators
        int viewX = cols - 60;
        if (viewX < 40) viewX = 40;

        mvprintw(0, viewX, "[1]Overview [2]Running [3]Pending [4]All");
        attroff(COLOR_PAIR(1) | A_BOLD);

        // Controls bar
        attron(COLOR_PAIR(1));
        mvhline(1, 0, ' ', cols);
        mvprintw(1, 2, "Controls: Up/Down:Scroll  Left/Right:Focus Column  PgUp/PgDn:Page  R:Refresh  Q:Quit");
        attroff(COLOR_PAIR(1));
    }

    void drawOverview() {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);

        int y = 3;

        attron(COLOR_PAIR(2) | A_BOLD);
        mvprintw(y++, 2, "JOB OVERVIEW");
        attroff(COLOR_PAIR(2) | A_BOLD);
        y++;

        mvprintw(y++, 4, "Total Jobs: %d", data.totalJobs);

        attron(COLOR_PAIR(3));
        mvprintw(y++, 4, "Running:    %d", data.runningJobs);
        attroff(COLOR_PAIR(3));

        attron(COLOR_PAIR(4));
        mvprintw(y++, 4, "Pending:    %d", data.pendingJobs);
        attroff(COLOR_PAIR(4));

        y += 2;

        if (!data.gpuTypeCount.empty()) {
            attron(COLOR_PAIR(2) | A_BOLD);
            mvprintw(y++, 2, "RUNNING - GPU ALLOCATIONS");
            attroff(COLOR_PAIR(2) | A_BOLD);
            y++;

            int totalGPUs = 0;
            for (const auto& pair : data.gpuTypeCount) {
                attron(COLOR_PAIR(3));
                mvprintw(y++, 4, "%-15s: %d GPUs", pair.first.c_str(), pair.second);
                attroff(COLOR_PAIR(3));
                totalGPUs += pair.second;
            }

            y++;
            attron(COLOR_PAIR(6) | A_BOLD);
            mvprintw(y++, 4, "Total Running:  %d GPUs", totalGPUs);
            attroff(COLOR_PAIR(6) | A_BOLD);

            y += 2;
        }

        if (!data.gpuTypeRequested.empty()) {
            attron(COLOR_PAIR(2) | A_BOLD);
            mvprintw(y++, 2, "PENDING - GPU REQUESTS");
            attroff(COLOR_PAIR(2) | A_BOLD);
            y++;

            int totalRequested = 0;
            for (const auto& pair : data.gpuTypeRequested) {
                attron(COLOR_PAIR(4));
                mvprintw(y++, 4, "%-15s: %d GPUs", pair.first.c_str(), pair.second);
                attroff(COLOR_PAIR(4));
                totalRequested += pair.second;
            }

            y++;
            attron(COLOR_PAIR(6) | A_BOLD);
            mvprintw(y++, 4, "Total Requested: %d GPUs", totalRequested);
            attroff(COLOR_PAIR(6) | A_BOLD);
        }
    }

    void drawJobTable(const std::vector<Job>& jobs, const std::string& title, int colorPair) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        maxRows = rows - 6; // Header + controls + title + table header + footer

        int y = 3;

        attron(COLOR_PAIR(2) | A_BOLD);
        mvprintw(y++, 2, "%s (%zu jobs)", title.c_str(), jobs.size());
        attroff(COLOR_PAIR(2) | A_BOLD);
        y++;

        // Calculate dynamic column widths
        int defaultWidths[8] = {10, 16, 16, 10, 10, 5, 10, 10};
        ColumnWidths w = calculateColumnWidths(cols, 8, defaultWidths, jobs, false);

        // Table header with dynamic widths and focus indicators
        const char* headers[8] = {"JobID", "JobName", "Account", "Runtime", "TimeLimit", "GPUs", "GPU Type", "Status"};
        int widths[8] = {w.jobId, w.jobName, w.account, w.col4, w.col5, w.col6, w.col7, w.col8};

        move(y, 0);
        clrtoeol();
        attron(A_BOLD);
        int xpos = 0;
        for (int i = 0; i < 8; i++) {
            move(y, xpos);
            // Add focus indicator
            if (focusedColumn == i) {
                attron(COLOR_PAIR(6)); // Red color for focused
                // Format: [HeaderName] constrained to column width
                char headerBuf[64];
                snprintf(headerBuf, sizeof(headerBuf), "[%s]", headers[i]);
                printw("%-*.*s", widths[i], widths[i], headerBuf);
                attroff(COLOR_PAIR(6));
            } else {
                // Format: HeaderName constrained to column width
                printw("%-*.*s", widths[i], widths[i], headers[i]);
            }
            xpos += widths[i] + 1; // +1 for space separator
        }
        attroff(A_BOLD);
        y++;

        // Build format string for data rows
        char fmt[256];
        snprintf(fmt, sizeof(fmt), "%%-%d.%ds %%-%d.%ds %%-%d.%ds %%-%d.%ds %%-%d.%ds %%-%d.%ds %%-%d.%ds %%-%d.%ds",
                 w.jobId, w.jobId, w.jobName, w.jobName, w.account, w.account,
                 w.col4, w.col4, w.col5, w.col5, w.col6, w.col6, w.col7, w.col7, w.col8, w.col8);

        // Table rows
        int displayedRows = 0;
        for (size_t i = scrollOffset; i < jobs.size() && displayedRows < maxRows; i++, displayedRows++) {
            const Job& job = jobs[i];

            attron(COLOR_PAIR(colorPair));

            // Prepare data fields (use full strings for focused column)
            std::string jobId = (focusedColumn == 0) ? job.jobId :
                               (job.jobId.length() > (size_t)w.jobId ? job.jobId.substr(0, w.jobId) : job.jobId);
            std::string jobName = (focusedColumn == 1) ? job.jobName :
                                 (job.jobName.length() > (size_t)w.jobName ? job.jobName.substr(0, w.jobName - 3) + "..." : job.jobName);
            std::string account = (focusedColumn == 2) ? job.account :
                                 (job.account.length() > (size_t)w.account ? job.account.substr(0, w.account - 3) + "..." : job.account);
            std::string runtime = (focusedColumn == 3) ? job.runtime :
                                 (job.runtime.length() > (size_t)w.col4 ? job.runtime.substr(0, w.col4) : job.runtime);
            std::string timeLimit = (focusedColumn == 4) ? job.timeLimit :
                                   (job.timeLimit.length() > (size_t)w.col5 ? job.timeLimit.substr(0, w.col5) : job.timeLimit);
            std::string gpuType = (focusedColumn == 6) ? job.gpuType :
                                 (job.gpuType.length() > (size_t)w.col7 ? job.gpuType.substr(0, w.col7 - 3) + "..." : job.gpuType);
            std::string state = (focusedColumn == 7) ? job.state :
                               (job.state.length() > (size_t)w.col8 ? job.state.substr(0, w.col8) : job.state);

            // Format entire line into buffer first with dynamic widths
            char fullLine[512];
            snprintf(fullLine, sizeof(fullLine), fmt,
                   jobId.c_str(),
                   jobName.c_str(),
                   account.c_str(),
                   runtime.c_str(),
                   timeLimit.c_str(),
                   std::to_string(job.gpuCount).c_str(),
                   job.gpuCount > 0 ? gpuType.c_str() : "N/A",
                   state.c_str());

            // HARD truncate to terminal width
            if ((int)strlen(fullLine) > cols - 2) {
                fullLine[cols - 2] = '\0';
            }

            move(y, 0);
            clrtoeol();
            addstr(fullLine);

            attroff(COLOR_PAIR(colorPair));
            y++;
        }

        // Scroll indicator
        if (jobs.size() > maxRows) {
            mvprintw(rows - 1, 2, "Showing %d-%zu of %zu (Scroll: %d%%)",
                     scrollOffset + 1,
                     std::min(scrollOffset + maxRows, (int)jobs.size()),
                     jobs.size(),
                     (int)((scrollOffset * 100) / std::max(1, (int)jobs.size() - maxRows)));
        }
    }

    void drawPendingView() {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        maxRows = rows - 6;

        int y = 3;

        std::vector<Job> pendingJobs;
        for (const auto& job : data.jobs) {
            if (job.state == "PENDING") {
                pendingJobs.push_back(job);
            }
        }

        // Sort by priority
        std::sort(pendingJobs.begin(), pendingJobs.end(),
                  [](const Job& a, const Job& b) { return a.priority > b.priority; });

        attron(COLOR_PAIR(2) | A_BOLD);
        mvprintw(y++, 2, "PENDING JOBS (%zu jobs)", pendingJobs.size());
        attroff(COLOR_PAIR(2) | A_BOLD);
        y++;

        // Calculate dynamic column widths (9 columns for pending view)
        int defaultWidths[9] = {10, 16, 16, 15, 10, 5, 10, 10, 8};
        ColumnWidths w = calculateColumnWidths(cols, 9, defaultWidths, pendingJobs, true);

        // Table header with dynamic widths and focus indicators
        const char* headers[9] = {"JobID", "JobName", "Account", "Reason", "TimeLimit", "GPUs", "GPU Type", "Priority", "Higher"};
        int widths[9] = {w.jobId, w.jobName, w.account, w.col4, w.col5, w.col6, w.col7, w.col8, w.col9};

        move(y, 0);
        clrtoeol();
        attron(A_BOLD);
        int xpos = 0;
        for (int i = 0; i < 9; i++) {
            move(y, xpos);
            // Add focus indicator
            if (focusedColumn == i) {
                attron(COLOR_PAIR(6)); // Red color for focused
                // Format: [HeaderName] constrained to column width
                char headerBuf[64];
                snprintf(headerBuf, sizeof(headerBuf), "[%s]", headers[i]);
                printw("%-*.*s", widths[i], widths[i], headerBuf);
                attroff(COLOR_PAIR(6));
            } else {
                // Format: HeaderName constrained to column width
                printw("%-*.*s", widths[i], widths[i], headers[i]);
            }
            xpos += widths[i] + 1; // +1 for space separator
        }
        attroff(A_BOLD);
        y++;

        // Build format string for data rows
        char fmt[256];
        snprintf(fmt, sizeof(fmt), "%%-%d.%ds %%-%d.%ds %%-%d.%ds %%-%d.%ds %%-%d.%ds %%-%d.%ds %%-%d.%ds %%-%d.%ds %%-%d.%ds",
                 w.jobId, w.jobId, w.jobName, w.jobName, w.account, w.account, w.col4, w.col4,
                 w.col5, w.col5, w.col6, w.col6, w.col7, w.col7, w.col8, w.col8, w.col9, w.col9);

        // Table rows
        int displayedRows = 0;
        for (size_t i = scrollOffset; i < pendingJobs.size() && displayedRows < maxRows; i++, displayedRows++) {
            const Job& job = pendingJobs[i];

            // Count jobs with higher priority in the overall queue
            int higherPriorityCount = 0;
            for (const auto& otherJob : data.allPendingJobs) {
                if (otherJob.priority > job.priority) {
                    higherPriorityCount++;
                }
            }

            // Prepare data fields (use full strings for focused column)
            std::string jobId = (focusedColumn == 0) ? job.jobId :
                               (job.jobId.length() > (size_t)w.jobId ? job.jobId.substr(0, w.jobId) : job.jobId);
            std::string jobName = (focusedColumn == 1) ? job.jobName :
                                 (job.jobName.length() > (size_t)w.jobName ? job.jobName.substr(0, w.jobName - 3) + "..." : job.jobName);
            std::string account = (focusedColumn == 2) ? job.account :
                                 (job.account.length() > (size_t)w.account ? job.account.substr(0, w.account - 3) + "..." : job.account);
            std::string reason = (focusedColumn == 3) ? job.reason :
                                (job.reason.length() > (size_t)w.col4 ? job.reason.substr(0, w.col4 - 3) + "..." : job.reason);
            std::string timeLimit = (focusedColumn == 4) ? job.timeLimit :
                                   (job.timeLimit.length() > (size_t)w.col5 ? job.timeLimit.substr(0, w.col5) : job.timeLimit);
            std::string gpuType = (focusedColumn == 6) ? job.gpuType :
                                 (job.gpuType.length() > (size_t)w.col7 ? job.gpuType.substr(0, w.col7 - 3) + "..." : job.gpuType);

            // Format and truncate numeric fields to prevent overflow
            char priorityStr[32], higherStr[32];
            snprintf(priorityStr, sizeof(priorityStr), "%ld", job.priority);
            snprintf(higherStr, sizeof(higherStr), "%d", higherPriorityCount);

            // Format entire line into buffer first with dynamic widths
            char fullLine[512];
            snprintf(fullLine, sizeof(fullLine), fmt,
                   jobId.c_str(),
                   jobName.c_str(),
                   account.c_str(),
                   reason.c_str(),
                   timeLimit.c_str(),
                   std::to_string(job.gpuCount).c_str(),
                   job.gpuCount > 0 ? gpuType.c_str() : "N/A",
                   priorityStr,
                   higherStr);

            // HARD truncate to terminal width
            if ((int)strlen(fullLine) > cols - 2) {
                fullLine[cols - 2] = '\0';
            }

            move(y, 0);
            clrtoeol();
            attron(COLOR_PAIR(4));
            addstr(fullLine);

            attroff(COLOR_PAIR(4));
            y++;
        }

        // Scroll indicator
        if (pendingJobs.size() > maxRows) {
            mvprintw(rows - 1, 2, "Showing %d-%zu of %zu (Scroll: %d%%)",
                     scrollOffset + 1,
                     std::min(scrollOffset + maxRows, (int)pendingJobs.size()),
                     pendingJobs.size(),
                     (int)((scrollOffset * 100) / std::max(1, (int)pendingJobs.size() - maxRows)));
        }
    }

    void draw() {
        erase();  // Clear the entire screen
        drawHeader();

        switch (currentView) {
            case OVERVIEW:
                drawOverview();
                break;
            case RUNNING: {
                std::vector<Job> runningJobs;
                for (const auto& job : data.jobs) {
                    if (job.state == "RUNNING") {
                        runningJobs.push_back(job);
                    }
                }
                drawJobTable(runningJobs, "RUNNING JOBS", 3);
                break;
            }
            case PENDING:
                drawPendingView();
                break;
            case ALL:
                drawJobTable(data.jobs, "ALL JOBS", 5);
                break;
        }

        refresh();
    }

    bool handleInput() {
        int ch = getch();

        if (ch == ERR) return false; // No input

        bool needRedraw = true;

        switch (ch) {
            case 'q':
            case 'Q':
                running = false;
                break;
            case 'r':
            case 'R':
                fetchSlurmData(data);
                scrollOffset = 0;
                break;
            case '1':
                currentView = OVERVIEW;
                scrollOffset = 0;
                focusedColumn = -1;
                break;
            case '2':
                currentView = RUNNING;
                scrollOffset = 0;
                focusedColumn = -1;
                break;
            case '3':
                currentView = PENDING;
                scrollOffset = 0;
                focusedColumn = -1;
                break;
            case '4':
                currentView = ALL;
                scrollOffset = 0;
                focusedColumn = -1;
                break;
            case KEY_UP:
                if (scrollOffset > 0) scrollOffset--;
                break;
            case KEY_DOWN:
                scrollOffset++;
                break;
            case KEY_LEFT:
                // Cycle focus left through columns (-1 means no focus)
                if (currentView != OVERVIEW) {
                    focusedColumn--;
                    int maxCol = (currentView == PENDING) ? 8 : 7;  // Pending has 9 columns (0-8), others have 8 (0-7)
                    if (focusedColumn < -1) focusedColumn = maxCol;
                }
                break;
            case KEY_RIGHT:
                // Cycle focus right through columns
                if (currentView != OVERVIEW) {
                    focusedColumn++;
                    int maxCol = (currentView == PENDING) ? 8 : 7;  // Pending has 9 columns (0-8), others have 8 (0-7)
                    if (focusedColumn > maxCol) focusedColumn = -1;
                }
                break;
            case KEY_PPAGE: // Page Up
                scrollOffset = std::max(0, scrollOffset - maxRows);
                break;
            case KEY_NPAGE: // Page Down
                scrollOffset += maxRows;
                break;
            case KEY_RESIZE:
                // Terminal was resized
                break;
            default:
                needRedraw = false;
                break;
        }

        // Limit scroll offset
        if (scrollOffset < 0) scrollOffset = 0;

        return needRedraw;
    }

    void run() {
        draw(); // Initial draw
        while (running) {
            if (handleInput()) {
                draw(); // Only redraw if input was handled
            }
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <username>" << std::endl;
        std::cerr << "\nControls:" << std::endl;
        std::cerr << "  1-4: Switch views (Overview/Running/Pending/All)" << std::endl;
        std::cerr << "  Up/Down: Scroll up/down" << std::endl;
        std::cerr << "  Left/Right: Focus column" << std::endl;
        std::cerr << "  PgUp/PgDn: Scroll by page" << std::endl;
        std::cerr << "  R: Refresh" << std::endl;
        std::cerr << "  Q: Quit" << std::endl;
        return 1;
    }

    SlurmData data;
    data.username = argv[1];

    // Initial data fetch
    fetchSlurmData(data);

    // Run UI
    SlurmTopUI ui(data);
    ui.run();

    return 0;
}
