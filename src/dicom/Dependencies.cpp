// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/Dependencies.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdict.h>
#include <dcmtk/dcmnet/assoc.h>

#include <memory>

namespace dingobench::dicom {

DependencyInfo dependencies()
{
    T_ASC_Parameters *parameters = nullptr;
    const auto status = ASC_createAssociationParameters(&parameters, ASC_DEFAULTMAXPDU, 1);
    const auto destroy = [](T_ASC_Parameters *value) { ASC_destroyAssociationParameters(&value); };
    const std::unique_ptr<T_ASC_Parameters, decltype(destroy)> owned(parameters, destroy);

    return {QStringLiteral(PACKAGE_VERSION PACKAGE_VERSION_SUFFIX),
            {QStringLiteral("dcmnet"), QStringLiteral("dcmdata"),
             QStringLiteral("ofstd"), QStringLiteral("oflog")},
            dcmDataDict.isDictionaryLoaded(), status.good() && owned != nullptr};
}

}  // namespace dingobench::dicom
