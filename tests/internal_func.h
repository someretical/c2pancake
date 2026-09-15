#include <stdint.h>

// this function prototype should have a wrapper func added
// the return type should be encoded in arg0
uintptr_t external_func1(uintptr_t a1, uintptr_t a2, uintptr_t a3);

// this function prototype should have a wrapper func added
// the return type should be encoded in arg0 as a pointer
// arg1 should be a pointer to a3-5
// arg2 and arg3 should be the values of a1 and a2
uintptr_t external_func2(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);

// this function prototype should have a wrapper func added
// the return type should be encoded in arg0 as a pointer
// arg1-3 should do nothing since there are no args to this function
uintptr_t external_func3(void);

// this function prototype should have a wrapper func added
// the return type should not be encoded in any argument
// arg0 and arg1 should be the values of a1 and a2 respectively
// arg2 and arg3 should do nothing since there are no more args to this function
void external_func4(uintptr_t a1, uintptr_t a2);

// this function prototype should have a wrapper func added
// the return type should not be encoded in any argument
// arg0 should be a pointer to a4-5
// arg1-3 should be the values of a1-3 respectively
void external_func5(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);

struct external_helper {
  uintptr_t a;
  uintptr_t b;
};
// this function should have a wrapper func added
// the return type should be encoded as a pointer to a struct external_helper in arg0
// arg1 should be a pointer to a struct array of a3-5
// arg2 and arg3 should be pointers to the values of a1 and a2

// TODO the generated code should use the __output type as the struct directly instead of casting it to uint8_t first...
struct external_helper external_func100(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);

// this function should have a wrapper func added
// the return type should be encoded as a pointer to a struct external_helper in arg0
// arg1 should be a pointer to a struct array of a3-5
// arg2 and arg3 should be pointers to the values of a1 and a2 (THIS IS NOT A THING YET!!!, at the moment it is all
// encoded in the struct array...)
struct external_helper external_func101(struct external_helper h1, struct external_helper h2, struct external_helper h3,
                                        struct external_helper h4, struct external_helper h5);

// this function should have a wrapper func added
// the return type should not be encoded in any argument
// arg0 should be a pointer to a struct array of a3-5
// arg1 and arg2 should be pointers to the values of a1 and a2
void external_func102(struct external_helper h1, struct external_helper h2, struct external_helper h3,
                      struct external_helper h4, struct external_helper h5);
