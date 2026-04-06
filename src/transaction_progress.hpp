// src/transaction_progress.hpp
#pragma once

#include <cstddef>
#include <string>

struct SearchWidgets;
struct TransactionPreview;
struct TransactionProgressWindow;

using TransactionApplyCallback = void (*)(SearchWidgets *widgets);

TransactionProgressWindow *transaction_progress_create_window(SearchWidgets *widgets, size_t pending_count);
void transaction_progress_append(TransactionProgressWindow *progress, const std::string &message);
void transaction_progress_finish(TransactionProgressWindow *progress, bool success, const std::string &summary);
void transaction_progress_show_error_dialog(SearchWidgets *widgets,
                                            const char *title,
                                            const char *intro,
                                            const std::string &details);
void transaction_progress_show_summary_dialog(SearchWidgets *widgets,
                                              const TransactionPreview &preview,
                                              TransactionApplyCallback on_apply);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
