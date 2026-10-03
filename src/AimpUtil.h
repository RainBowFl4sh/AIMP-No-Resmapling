// Small helpers for the AIMP interfaces (reference counting, strings).
#pragma once
#include <atomic>
#include <initializer_list>

#include "Common.h"
#include "apiCore.h"
#include "apiObjects.h"

namespace ar {

// Minimal smart pointer for IUnknown-based AIMP objects
template <typename T>
class Ptr {
    T* p_ = nullptr;

public:
    Ptr() = default;
    Ptr(const Ptr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Ptr(Ptr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Ptr() { Reset(); }
    Ptr& operator=(Ptr o) { std::swap(p_, o.p_); return *this; }

    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    // For out parameters: releases the old object and returns the address
    T** Out() { Reset(); return &p_; }
    void** OutV() { Reset(); return (void**)&p_; }
};

// IUnknown implementation; Supports() lists the additional interfaces.
// AIMP convention: the counter starts at 0, the receiver calls AddRef.
template <typename Base>
class ComObject : public Base {
    std::atomic<long> refs_{0};

protected:
    virtual bool Supports(REFIID riid) { (void)riid; return false; }

public:
    virtual ~ComObject() {}

    HRESULT __unknwncall QueryInterface(REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (EqualGUID(riid, IID_IUnknown) || Supports(riid)) {
            *ppv = static_cast<Base*>(this);
            this->AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    DWORD __unknwncall AddRef() override { return (DWORD)++refs_; }
    DWORD __unknwncall Release() override {
        long r = --refs_;
        if (r == 0) delete this;
        return (DWORD)r;
    }
};

inline bool IsAny(REFIID riid, std::initializer_list<const GUID*> ids) {
    for (const GUID* g : ids)
        if (EqualGUID(riid, *g)) return true;
    return false;
}

// Global core pointer, set in Initialize
inline IAIMPCore*& Core() {
    static IAIMPCore* c = nullptr;
    return c;
}

inline Ptr<IAIMPString> MakeString(const tstring& s) {
    Ptr<IAIMPString> r;
    if (Core() && Succeeded(Core()->CreateObject(IID_IAIMPString, r.OutV())) && r)
        r->SetData(const_cast<PChar>(s.c_str()), (INT32)s.size());
    return r;
}

inline tstring FromString(IAIMPString* s) {
    if (!s || !s->GetData()) return tstring();
    return tstring(s->GetData(), (size_t)s->GetLength());
}

template <typename T>
inline Ptr<T> Service(const GUID& iid) {
    Ptr<T> r;
    if (Core()) Core()->QueryInterface(iid, r.OutV());
    return r;
}

}  // namespace ar
