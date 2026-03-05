#include <iostream>

#include "MySampleApp.h"

int main()
{
    try
    {
        MySampleApp app;
        app.Run();
    }
    catch (const std::exception &e)
    {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cerr << "Unhandled exception: " << e.what() << std::endl;
#endif
        return 1;
    }
    return 0;
}