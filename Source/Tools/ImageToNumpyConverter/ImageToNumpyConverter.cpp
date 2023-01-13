#include "convert_to_numpy.h"

int main(int argc, char* argv[])
{
    if (argc < 6)
    {
        std::cout << "Usage: ImageToNumpyConverter.exe <raster image> <ray image> <instances> <instance_center> <inScreen>" << std::endl;
        return 1;
    }

    convert_to_numpy(argv[1], argv[2], /*argv[3], argv[4],*/ argv[5]);
    
    return 0;
}
