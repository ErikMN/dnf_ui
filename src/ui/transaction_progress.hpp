// src/ui/transaction_progress.hpp
#pragma once

#include <cstddef>
#include <string>

struct SearchWidgets;
struct TransactionPreview;
struct TransactionProgressWindow;

using TransactionApplyCallback = void (*)(SearchWidgets *widgets);

// -----------------------------------------------------------------------------
// transaction_progress_create_window
// -----------------------------------------------------------------------------
TransactionProgressWindow *transaction_progress_create_window(SearchWidgets *widgets, size_t pending_count);
// -----------------------------------------------------------------------------
// transaction_progress_retain
// -----------------------------------------------------------------------------
TransactionProgressWindow *transaction_progress_retain(TransactionProgressWindow *progress);
// -----------------------------------------------------------------------------
// transaction_progress_release
// -----------------------------------------------------------------------------
void transaction_progress_release(TransactionProgressWindow *progress);
// -----------------------------------------------------------------------------
// transaction_progress_append
// -----------------------------------------------------------------------------
void transaction_progress_append(TransactionProgressWindow *progress, const std::string &message);
// -----------------------------------------------------------------------------
// transaction_progress_finish
// -----------------------------------------------------------------------------
void transaction_progress_finish(TransactionProgressWindow *progress, bool success, const std::string &summary);
// -----------------------------------------------------------------------------
// transaction_progress_show_error_dialog
// -----------------------------------------------------------------------------
void transaction_progress_show_error_dialog(SearchWidgets *widgets,
                                            const char *title,
                                            const char *intro,
                                            const std::string &details);
// -----------------------------------------------------------------------------
// transaction_progress_show_summary_dialog
// -----------------------------------------------------------------------------
void transaction_progress_show_summary_dialog(SearchWidgets *widgets,
                                              const TransactionPreview &preview,
                                              TransactionApplyCallback on_apply,
                                              TransactionApplyCallback on_cancel);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
