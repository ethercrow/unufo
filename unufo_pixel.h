namespace unufo {


inline int pixel_diff(const uint8_t p1, const uint8_t p2)
{
    int result = p1;
    result -= p2;
    return result*result;
}

const int max_diff = 256*256;

}

