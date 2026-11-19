#pragma once

#include <QString>

namespace knurbs::log {

// Opens a new log file in the given directory and routes libobs' blog() and
// Qt's messages to it. Keeps the newest `keep` log files.
bool open(const QString &directory, bool verbose, int keep = 10);

// Path of the log file this session writes to.
QString currentFile();

void close();

} // namespace knurbs::log
