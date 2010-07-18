#ifndef UNUFO_UTILS_H_
#define UNUFO_UTILS_H_

#include <stdio.h>
#include <string>

#ifndef NDEBUG

    #define UNUFO_LOG(...) fprintf(stderr, __VA_ARGS__);

    #define UNUFO_SCOPED_LOG\
        unufo_scoped_logger scoped_logger{__func__};

#else
    #define UNUFO_LOG(...)
    #define UNUFO_SCOPED_LOG
#endif

class unufo_scoped_logger
{
public:
    unufo_scoped_logger(const char* const name): name_{name}
    { UNUFO_LOG("%s begin\n", name_.c_str()); }

    ~unufo_scoped_logger()
    { UNUFO_LOG("%s end\n", name_.c_str()); }

private:
    const std::string name_;
};


#endif

