#include "LibetpanBackendFactory.h"

#ifdef NINGMAIL_WITH_LIBETPAN

#include "LibetpanBackend.h"

namespace ningmail {

std::unique_ptr<MailBackend> LibetpanBackendFactory::create(const Account &account) {
    // The account travels to the backend through connect(), so a creation
    // parameter would only be state; keep the interface signature.
    Q_UNUSED(account)
    return std::make_unique<LibetpanBackend>();
}

} // namespace ningmail

#endif // NINGMAIL_WITH_LIBETPAN
