// -----------------------------------------------------------------------------
// transaction_service_preview_formatter.cpp
// Transaction service preview text formatting
// Keeps human-readable preview text separate from D-Bus request handling.
// -----------------------------------------------------------------------------
#include "service/transaction_service_preview_formatter.hpp"

#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"

#include <glib.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Format the resolved disk space change for the transaction summary text.
// -----------------------------------------------------------------------------
static std::string
format_transaction_preview_space_change(long long delta_bytes)
{
  if (delta_bytes == 0) {
    return _("Disk space usage will be unchanged.");
  }

  unsigned long long abs_bytes =
      delta_bytes > 0 ? static_cast<unsigned long long>(delta_bytes) : static_cast<unsigned long long>(-delta_bytes);
  char *formatted = g_format_size(abs_bytes);
  std::string line;

  if (delta_bytes > 0) {
    line = dnfui_i18n_format(_("%s extra disk space will be used."), formatted);
  } else {
    line = dnfui_i18n_format(_("%s of disk space will be freed."), formatted);
  }

  g_free(formatted);
  return line;
}

// -----------------------------------------------------------------------------
// Append one resolved package section to the transaction summary text.
// -----------------------------------------------------------------------------
static void
append_transaction_preview_section(std::ostringstream &summary,
                                   const char *title,
                                   const std::vector<std::string> &items)
{
  if (!title || items.empty()) {
    return;
  }

  summary << title << ":\n";
  for (const auto &item : items) {
    summary << "  " << item << "\n";
  }
  summary << "\n";
}

// -----------------------------------------------------------------------------
// Format the full resolved transaction preview as a readable summary string.
// -----------------------------------------------------------------------------
std::string
format_transaction_preview_details(const TransactionPreview &preview)
{
  std::ostringstream summary;

  if (preview.empty()) {
    summary << _("No package changes are available.") << "\n";
  }

  auto append_count_line = [&](size_t count, const char *singular, const char *plural) {
    if (count == 0) {
      return;
    }
    summary << dnfui_i18n_format_count(count, singular, plural) << "\n";
  };

  append_count_line(preview.install.size(), "%zu package will be installed.", "%zu packages will be installed.");
  append_count_line(preview.upgrade.size(), "%zu package will be upgraded.", "%zu packages will be upgraded.");
  append_count_line(preview.downgrade.size(), "%zu package will be downgraded.", "%zu packages will be downgraded.");
  append_count_line(preview.reinstall.size(), "%zu package will be reinstalled.", "%zu packages will be reinstalled.");
  append_count_line(preview.remove.size(), "%zu package will be removed.", "%zu packages will be removed.");
  summary << format_transaction_preview_space_change(preview.disk_space_delta) << "\n\n";

  append_transaction_preview_section(summary, _("To be installed"), preview.install);
  append_transaction_preview_section(summary, _("To be upgraded"), preview.upgrade);
  append_transaction_preview_section(summary, _("To be downgraded"), preview.downgrade);
  append_transaction_preview_section(summary, _("To be reinstalled"), preview.reinstall);
  append_transaction_preview_section(summary, _("To be removed"), preview.remove);

  return summary.str();
}
