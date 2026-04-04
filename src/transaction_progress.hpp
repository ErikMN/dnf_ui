// src/transaction_progress.hpp
#pragma once

#include <cstddef>
#include <string>

struct SearchWidgets;
struct TransactionPreview;
struct TransactionProgressWindow;

using TransactionApplyCallback = void (*)(SearchWidgets *widgets);

TransactionProgressWindow *create_transaction_progress_window(SearchWidgets *widgets, size_t pending_count);
void append_transaction_progress(TransactionProgressWindow *progress, const std::string &message);
void finish_transaction_progress(TransactionProgressWindow *progress, bool success, const std::string &summary);
void
show_transaction_error_dialog(SearchWidgets *widgets, const char *title, const char *intro, const std::string &details);
void show_transaction_summary_dialog(SearchWidgets *widgets,
                                     const TransactionPreview &preview,
                                     TransactionApplyCallback on_apply);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
