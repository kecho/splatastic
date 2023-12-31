#include <files/Utils.h>
#include <utils/String.h>

namespace splatastic
{

FileLookup::FileLookup()
: filename(""), hash(0u)
{
}

FileLookup::FileLookup(const char* file)
: filename(file)
{
    hash = stringHash(filename);
}

FileLookup::FileLookup(const std::string& filename)
: filename(filename)
{
    hash = stringHash(filename);
}

}
