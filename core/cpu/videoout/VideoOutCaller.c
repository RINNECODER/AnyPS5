#include <stdint.h>

// Synthetic typed SysV callers. The compiler owns register narrowing, stack
// arguments and CALL/RET. These are public fixture inputs, never retail bytes.
struct Arguments { uint64_t value[8]; };
int32_t videoout_open(void* function, const struct Arguments* a) {
    typedef int32_t (*Open)(int32_t,int32_t,int32_t,const void*);
    return ((Open)function)((int32_t)a->value[0],(int32_t)a->value[1],
        (int32_t)a->value[2],(const void*)a->value[3]);
}
int32_t videoout_status(void* function, const struct Arguments* a) {
    typedef int32_t (*Status)(int32_t,void*);
    return ((Status)function)((int32_t)a->value[0],(void*)a->value[1]);
}
int32_t videoout_close(void* function, const struct Arguments* a) {
    typedef int32_t (*Close)(int32_t);
    return ((Close)function)((int32_t)a->value[0]);
}
void videoout_attribute(void* function, const struct Arguments* a) {
    typedef void (*Attribute)(void*,uint64_t,uint32_t,uint32_t,uint32_t,uint64_t,uint32_t,uint64_t);
    ((Attribute)function)((void*)a->value[0],a->value[1],(uint32_t)a->value[2],
        (uint32_t)a->value[3],(uint32_t)a->value[4],a->value[5],(uint32_t)a->value[6],a->value[7]);
}
int32_t videoout_register(void* function, const struct Arguments* a) {
    typedef int32_t (*Register)(int32_t,int32_t,int32_t,const void*,int32_t,const void*,int32_t,const void*);
    return ((Register)function)((int32_t)a->value[0],(int32_t)a->value[1],
        (int32_t)a->value[2],(const void*)a->value[3],(int32_t)a->value[4],
        (const void*)a->value[5],(int32_t)a->value[6],(const void*)a->value[7]);
}
int32_t videoout_rate(void* function, const struct Arguments* a) {
    typedef int32_t (*Rate)(int32_t,int32_t);
    return ((Rate)function)((int32_t)a->value[0],(int32_t)a->value[1]);
}
int32_t videoout_unregister(void* function, const struct Arguments* a) {
    typedef int32_t (*Unregister)(int32_t,int32_t);
    return ((Unregister)function)((int32_t)a->value[0],(int32_t)a->value[1]);
}
