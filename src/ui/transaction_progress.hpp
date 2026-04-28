// src/ui/transaction_progress.hpp
// Transaction progress window helpers
//
// Creates the apply progress dialog and keeps it alive while service progress
// callbacks are still pending.
#pragma once

#include <cstddef>
#include <string>

struct SearchWidgets;
struct TransactionPreview;
struct TransactionProgressWindow;

using TransactionApplyCallback = void (*)(SearchWidgets *widgets);

// -----------------------------------------------------------------------------
// Create a transaction progress window for the pending action count.
// -----------------------------------------------------------------------------
TransactionProgressWindow *transaction_progress_create_window(SearchWidgets *widgets, size_t pending_count);
// -----------------------------------------------------------------------------
// Add one reference to a transaction progress window.
// -----------------------------------------------------------------------------
TransactionProgressWindow *transaction_progress_retain(TransactionProgressWindow *progress);
// -----------------------------------------------------------------------------
// Release one reference to a transaction progress window.
// -----------------------------------------------------------------------------
void transaction_progress_release(TransactionProgressWindow *progress);
// -----------------------------------------------------------------------------
// Append a progress message to the transaction progress window.
// -----------------------------------------------------------------------------
void transaction_progress_append(TransactionProgressWindow *progress, const std::string &message);
// -----------------------------------------------------------------------------
// Mark the transaction progress window as finished.
// -----------------------------------------------------------------------------
void transaction_progress_finish(TransactionProgressWindow *progress, bool success, const std::string &summary);
// -----------------------------------------------------------------------------
// Show a transaction error dialog with optional details.
// -----------------------------------------------------------------------------
void transaction_progress_show_error_dialog(SearchWidgets *widgets,
                                            const char *title,
                                            const char *intro,
                                            const std::string &details);
// -----------------------------------------------------------------------------
// Show the resolved transaction summary before apply.
// -----------------------------------------------------------------------------
void transaction_progress_show_summary_dialog(SearchWidgets *widgets,
                                              const TransactionPreview &preview,
                                              TransactionApplyCallback on_apply,
                                              TransactionApplyCallback on_cancel);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
