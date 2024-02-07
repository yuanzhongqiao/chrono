// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2014 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================

#ifndef CHARCHIVEASCIIDUMP_H
#define CHARCHIVEASCIIDUMP_H

#include "chrono/serialization/ChArchive.h"
#include <cstring>

namespace chrono {

/// ASCII 'LOG' ARCHIVES (only output, for debugging etc.)

///
/// This is a class for serializing to ascii logging
///

class ChArchiveAsciiDump : public ChArchiveOut {
  public:
    ChArchiveAsciiDump(std::ostream& mostream) {
        ostream = &mostream;
        tablevel = 0;
        use_versions = false;
        suppress_names = false;
    }

    virtual ~ChArchiveAsciiDump() {}

    /// Suppress export of variable names
    void SetSuppressNames(bool msu) { suppress_names = msu; }

    /// Access the stream used by the archive.
    std::ostream* GetStream() { return ostream; }

    void indent() {
        for (int i = 0; i < tablevel; ++i)
            (*ostream) << "\t";
    }

    virtual void out(ChNameValue<bool> bVal) {
        indent();
        if (!suppress_names)
            (*ostream) << bVal.name() << "\t";
        (*ostream) << bVal.value();
        (*ostream) << "\n";
    }
    virtual void out(ChNameValue<int> bVal) {
        indent();
        if (!suppress_names)
            (*ostream) << bVal.name() << "\t";
        (*ostream) << bVal.value();
        (*ostream) << "\n";
    }
    virtual void out(ChNameValue<double> bVal) {
        indent();
        if (!suppress_names)
            (*ostream) << bVal.name() << "\t";
        (*ostream) << bVal.value();
        (*ostream) << "\n";
    }
    virtual void out(ChNameValue<float> bVal) {
        indent();
        if (!suppress_names)
            (*ostream) << bVal.name() << "\t";
        (*ostream) << bVal.value();
        (*ostream) << "\n";
    }
    virtual void out(ChNameValue<char> bVal) {
        indent();
        (*ostream) << bVal.name();
        (*ostream) << "\t";
        (*ostream) << bVal.value();
        (*ostream) << "\n";
    }
    virtual void out(ChNameValue<unsigned int> bVal) {
        indent();
        if (!suppress_names)
            (*ostream) << bVal.name() << "\t";
        (*ostream) << bVal.value();
        (*ostream) << "\n";
    }
    virtual void out(ChNameValue<std::string> bVal) {
        indent();
        if (!suppress_names)
            (*ostream) << bVal.name() << "\t";
        (*ostream) << "\"";
        (*ostream) << bVal.value();
        (*ostream) << "\"\n";
    }
    virtual void out(ChNameValue<unsigned long> bVal) {
        indent();
        if (!suppress_names)
            (*ostream) << bVal.name() << "\t";
        (*ostream) << bVal.value();
        (*ostream) << "\n";
    }
    virtual void out(ChNameValue<unsigned long long> bVal) {
        indent();
        if (!suppress_names)
            (*ostream) << bVal.name() << "\t";
        (*ostream) << bVal.value();
        (*ostream) << "\n";
    }
    virtual void out(ChNameValue<ChEnumMapperBase> bVal) {
        indent();
        if (!suppress_names)
            (*ostream) << bVal.name() << "\t";
        (*ostream) << "\"";
        std::string mstr = bVal.value().GetValueAsString();
        (*ostream) << mstr;
        (*ostream) << "\"\n";
    }

    virtual void out_array_pre(ChValue& bVal, size_t msize) {
        indent();
        if (!suppress_names) {
            (*ostream) << bVal.name() << "  ";
        }
        (*ostream) << "container of " << msize << " items, [" << bVal.GetTypeidName() << "]\n";
        ++tablevel;
        indent();
        (*ostream) << "[ \n";
        ++tablevel;
    }
    virtual void out_array_between(ChValue& bVal, size_t msize) {}
    virtual void out_array_end(ChValue& bVal, size_t msize) {
        --tablevel;
        indent();
        (*ostream) << "] \n";
        --tablevel;
    }

    // for custom c++ objects:
    virtual void out(ChValue& bVal, bool tracked, size_t obj_ID) {
        indent();
        if (!suppress_names)
            (*ostream) << bVal.name() << "  ";
        (*ostream) << "[" << bVal.GetTypeidName() << "]";
        if (tracked)
            (*ostream) << " (tracked)   ID= " << obj_ID;
        if (this->use_versions)
            (*ostream) << " version=" << bVal.GetClassRegisteredVersion();
        (*ostream) << " \n";
        ++tablevel;
        bVal.CallArchiveOut(*this);
        --tablevel;
    }

    virtual void out_ref(ChValue& bVal, bool already_inserted, size_t obj_ID, size_t ext_ID) {
        const char* classname = bVal.GetClassRegisteredName().c_str();
        indent();
        if (!suppress_names)
            (*ostream) << bVal.name();
        (*ostream) << "->";
        if (strlen(classname) > 0) {
            (*ostream) << " [" << classname << "] (registered type)";
        } else {
            (*ostream) << " [" << bVal.GetTypeidName() << "]";
        }
        if (obj_ID)
            (*ostream) << "  ID=" << obj_ID;
        if (ext_ID)
            (*ostream) << "  external_ID=" << ext_ID;
        if (this->use_versions)
            (*ostream) << " version=" << bVal.GetClassRegisteredVersion();
        (*ostream) << "\n";
        ++tablevel;
        if (!already_inserted) {
            if (!bVal.IsNull()) {
                // New Object, we have to full serialize it
                bVal.CallArchiveOut(*this);
            } else {
                (*ostream) << "NULL\n";
            }
        }
        --tablevel;
    }

  protected:
    int tablevel;
    std::ostream* ostream;
    bool suppress_names;
};

}  // end namespace chrono

#endif
