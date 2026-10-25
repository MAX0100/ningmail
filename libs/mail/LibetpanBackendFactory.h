// Creates the real libetpan backend (task 13). Compiled only together with
// libetpan, so the app can keep the fake backend as fallback (demo mode).

#pragma once

#include "app/core/MailBackend.h"

#include <memory>

#ifdef NINGMAIL_WITH_LIBETPAN

namespace ningmail {

/// Produces LibetpanBackend instances for the configured account.
class LibetpanBackendFactory final : public MailBackendFactory {
  public:
    std::unique_ptr<MailBackend> create(const Account &account) override;
};

} // namespace ningmail

#endif // NINGMAIL_WITH_LIBETPAN
