// C++98 constraint: classic unions cannot hold members with non-trivial
// constructors/destructors (e.g. std::string). We therefore emulate storage
// using raw, properly aligned character buffers and explicit placement new.

#include <cassert>
#include <new>

namespace util {

// Helper for aligned raw storage sufficient for type T (coarse alignment).
template <typename T>
struct RawStorage {
  union U {
    char data[sizeof(T)];
    void *p;
    long double ld;
  } u;  // alignment proxy
  void *Ptr() { return static_cast<void *>(u.data); }
  const void *Ptr() const { return static_cast<const void *>(u.data); }
  T *Obj() { return reinterpret_cast<T *>(Ptr()); }
  const T *Obj() const { return reinterpret_cast<const T *>(Ptr()); }
};

// ----------------------------- Option<T> ---------------------------------- //
template <typename T>
class Option {
 public:
  Option() : m_has(false) {}
  explicit Option(const T &value) : m_has(true) {
    new (m_store.Ptr()) T(value);
  }
  Option(const Option &other) : m_has(false) { CopyFrom(other); }
  Option &operator=(const Option &other) {
    if (this != &other) AssignFrom(other);
    return *this;
  }
  ~Option() { Reset(); }

  static Option Some(const T &value) { return Option(value); }
  static Option None() { return Option(); }

  bool IsSome() const { return m_has; }
  bool IsNone() const { return !m_has; }
  bool HasValue() const { return m_has; }

  void Reset() {
    if (m_has) {
      m_store.Obj()->~T();
      m_has = false;
    }
  }

  void Replace(const T &value) {
    if (m_has) {
      *m_store.Obj() = value;
    } else {
      new (m_store.Ptr()) T(value);
      m_has = true;
    }
  }

  T &Value() {
    assert(m_has);
    return *m_store.Obj();
  }
  const T &Value() const {
    assert(m_has);
    return *m_store.Obj();
  }
  T ValueOr(const T &fallback) const {
    return m_has ? *m_store.Obj() : fallback;
  }

 private:
  void CopyFrom(const Option &other) {
    if (other.m_has) {
      new (m_store.Ptr()) T(*other.m_store.Obj());
      m_has = true;
    }
  }
  void AssignFrom(const Option &other) {
    if (m_has && other.m_has) {
      *m_store.Obj() = *other.m_store.Obj();
    } else if (!m_has && other.m_has) {
      new (m_store.Ptr()) T(*other.m_store.Obj());
      m_has = true;
    } else if (m_has && !other.m_has) {
      m_store.Obj()->~T();
      m_has = false;
    }
  }
  bool m_has;
  RawStorage<T> m_store;
};

// --------------------------- Result<T,E> ---------------------------------- //
template <typename T, typename E>
class Result {
 public:
  enum State { ST_OK, ST_ERR };
  static Result MakeOk(const T &value) { return Result(TagOk(), value); }
  static Result MakeErr(const E &error) { return Result(TagErr(), error); }

  Result() : m_state(ST_ERR), m_has(false) {}
  Result(const Result &other) : m_state(ST_ERR), m_has(false) {
    CopyFrom(other);
  }
  Result &operator=(const Result &other) {
    if (this != &other) AssignFrom(other);
    return *this;
  }
  ~Result() { Destroy(); }

  bool IsOk() const { return m_state == ST_OK; }
  bool IsErr() const { return m_state == ST_ERR; }
  T &Ok() {
    assert(IsOk());
    return *OkPtr();
  }
  const T &Ok() const {
    assert(IsOk());
    return *OkPtr();
  }
  E &Err() {
    assert(IsErr());
    return *ErrPtr();
  }
  const E &Err() const {
    assert(IsErr());
    return *ErrPtr();
  }
  T OkOr(const T &fallback) const { return IsOk() ? *OkPtr() : fallback; }

 private:
  struct TagOk {};
  struct TagErr {};
  // Raw storage large enough for either T or E.
  union StorageUnion {
    char data[(sizeof(T) > sizeof(E)) ? sizeof(T) : sizeof(E)];
    void *p;
    long double ld;
  } m_store;

  Result(TagOk, const T &value) : m_state(ST_OK), m_has(true) {
    new (m_store.data) T(value);
  }
  Result(TagErr, const E &error) : m_state(ST_ERR), m_has(true) {
    new (m_store.data) E(error);
  }

  T *OkPtr() { return reinterpret_cast<T *>(m_store.data); }
  const T *OkPtr() const { return reinterpret_cast<const T *>(m_store.data); }
  E *ErrPtr() { return reinterpret_cast<E *>(m_store.data); }
  const E *ErrPtr() const { return reinterpret_cast<const E *>(m_store.data); }

  void Destroy() {
    if (m_has) {
      if (m_state == ST_OK)
        OkPtr()->~T();
      else
        ErrPtr()->~E();
      m_has = false;
    }
  }
  void CopyFrom(const Result &other) {
    if (!other.m_has) return;
    m_state = other.m_state;
    m_has = true;
    if (other.m_state == ST_OK)
      new (m_store.data) T(*other.OkPtr());
    else
      new (m_store.data) E(*other.ErrPtr());
  }
  void AssignFrom(const Result &other) {
    if (m_state == other.m_state) {
      if (m_state == ST_OK) {
        if (m_has && other.m_has)
          *OkPtr() = *other.OkPtr();
        else if (!m_has && other.m_has) {
          new (m_store.data) T(*other.OkPtr());
          m_has = true;
        } else if (m_has && !other.m_has) {
          OkPtr()->~T();
          m_has = false;
        }
      } else {  // ST_ERR
        if (m_has && other.m_has)
          *ErrPtr() = *other.ErrPtr();
        else if (!m_has && other.m_has) {
          new (m_store.data) E(*other.ErrPtr());
          m_has = true;
        } else if (m_has && !other.m_has) {
          ErrPtr()->~E();
          m_has = false;
        }
      }
    } else {
      Destroy();
      CopyFrom(other);
    }
  }
  State m_state;
  bool m_has;
};

}  // namespace util
