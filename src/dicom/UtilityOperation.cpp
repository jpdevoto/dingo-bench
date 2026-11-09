// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/UtilityOperation.h"

#include <QDir>
#include <QProcess>

#include <algorithm>
#include <utility>

namespace dingobench::dicom {
using namespace std::chrono_literals;

Operation utilityOperation(QString executable, QStringList arguments,
                           std::chrono::milliseconds timeout)
{
    return [executable = std::move(executable), arguments = std::move(arguments), timeout](
               std::stop_token stop, const ReportDiagnostic &) -> OperationResult {
        if (!QDir::isAbsolutePath(executable) || timeout <= 0ms) {
            return OperationResult::failed(
                {ErrorCode::InvalidInput, QStringLiteral("The utility needs an absolute executable path and a positive timeout.")});
        }
        if (stop.stop_requested())
            return OperationResult::cancelled();

        QProcess process;
        process.setStandardInputFile(QProcess::nullDevice());
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setStandardErrorFile(QProcess::nullDevice());
        const auto startedAt = std::chrono::steady_clock::now();
        process.start(executable, arguments);

        // Poll in the worker so cancellation also works while the child starts.
        while (process.state() != QProcess::NotRunning) {
            const auto elapsed = std::chrono::steady_clock::now() - startedAt;
            if (stop.stop_requested() || elapsed >= timeout) {
                // Resolve a pending launch before trying to terminate the child.
                if (process.state() == QProcess::Starting)
                    process.waitForStarted(250);
                process.terminate();
                if (!process.waitForFinished(250) && process.state() != QProcess::NotRunning) {
                    process.kill();
                    process.waitForFinished(1000);
                }
                if (stop.stop_requested())
                    return OperationResult::cancelled();
                return OperationResult::failed(
                    {ErrorCode::TimedOut, QStringLiteral("The utility exceeded its time limit.")});
            }

            const auto wait = std::chrono::ceil<std::chrono::milliseconds>(timeout - elapsed);
            const auto interval = static_cast<int>(std::min(wait, 25ms).count());
            if (process.state() == QProcess::Starting)
                process.waitForStarted(interval);
            else
                process.waitForFinished(interval);
        }

        if (process.error() == QProcess::FailedToStart) {
            return OperationResult::failed(
                {ErrorCode::StartFailed, QStringLiteral("The utility could not start.")});
        }
        if (process.exitStatus() != QProcess::NormalExit) {
            return OperationResult::failed(
                {ErrorCode::ProcessFailed, QStringLiteral("The utility exited unexpectedly.")});
        }
        if (process.exitCode() != 0) {
            return OperationResult::failed(
                {ErrorCode::ProcessFailed, QStringLiteral("The utility returned a nonzero exit code."),
                 process.exitCode()});
        }
        return {};
    };
}

}  // namespace dingobench::dicom
