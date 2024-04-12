#ifndef TCLHBASE_H
#define TCLHBASE_H

/* Common definitions included by all Tcl helper *implemenations* */

#ifndef TCLH_INLINE
#ifdef _MSC_VER
# define TCLH_INLINE __inline
#else
# define TCLH_INLINE static inline
#endif
#endif

#ifndef TCLH_PANIC
#define TCLH_PANIC Tcl_Panic
#endif

#ifndef TCLH_ASSERT
# ifdef NDEBUG
#  define TCLH_ASSERT(bool_) (void) 0
# else
#  define TCLH_ASSERT(bool_) (void)( (bool_) || (TCLH_PANIC("Assertion (%s) failed at line %d in file %s.", #bool_, __LINE__, __FILE__), 0) )
# endif
#endif

#ifdef TCLH_IMPL
#ifndef TCLH_EMBEDDER
#error TCLH_EMBEDDER not defined. Please #define this to name of your package.
#endif
#endif

/*
 * Typedef: Tclh_SSizeT
 * This typedef is used to store max lengths of Tcl strings.
 * Its use is primarily to avoid compiler warnings with downcasting from size_t.
 */
#if TCL_MAJOR_VERSION <= 8
typedef int Tclh_SSizeT;
#else
typedef Tcl_Size Tclh_SSizeT;
#endif

TCLH_INLINE char *Tclh_memdup(void *from, Tclh_SSizeT len) {
    void *to = ckalloc(len);
    memcpy(to, from, len);
    return to;
}

TCLH_INLINE Tclh_SSizeT Tclh_strlen(const char *s) {
    return (Tclh_SSizeT) strlen(s);
}

TCLH_INLINE char *Tclh_strdup(const char *from) {
    Tclh_SSizeT len = Tclh_strlen(from) + 1;
    char *to = ckalloc(len);
    memcpy(to, from, len);
    return to;
}

TCLH_INLINE char *Tclh_strdupn(const char *from, Tclh_SSizeT len) {
    char *to = ckalloc(len+1);
    memcpy(to, from, len);
    to[len] = '\0';
    return to;
}



#endif /* TCLHBASE_H */