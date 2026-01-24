#include <cstdio>
#include <cassert>
#include <cstring>
#include <string>

#include <io.hpp>

int main()
{
    using buxtehude::Stream, buxtehude::Field;

    // (1) Test EOF marking
    {
        char buffer[] = "Ein feste Burg ist unser Gott";
        FILE* file = fmemopen(buffer, sizeof(buffer), "r");

        Stream stream(file);
        assert(stream.Status() == buxtehude::StreamStatus::OKAY);

        stream.Await(sizeof(buffer));
        assert(stream.Read() == true);
        assert(stream.Done() == true);
        assert(stream.Status() == buxtehude::StreamStatus::OKAY);

        auto [data, size] = stream[0].GetPtr<char>();
        assert(size == sizeof(buffer));
        assert(strcmp(buffer, data) == 0);

        assert(stream.Read() == false);
        assert(stream.Status() == buxtehude::StreamStatus::REACHED_EOF);

        fclose(file);
    }

    // (2) 'Then' & 'Finally' callbacks and creating callbacks within callbacks
    {
        uint16_t buffer[] = { 1, 6, 3, 7 };
        FILE* file = fmemopen(buffer, sizeof(buffer), "r");

        uint16_t pair1 = 0, pair2 = 0;

        Stream stream(file);
        stream.Await<uint16_t>().Await<uint16_t>()
            .Then([&pair1, &pair2] (Stream& s, Field& f) {
                pair1 = f.Get<uint16_t>() + f[-1].Get<uint16_t>();
                s.Finally([&pair2] (Stream& s, Field& f) {
                    pair2 = f.Get<uint16_t>() + f[-1].Get<uint16_t>();
                });
            }
        ).Await<uint16_t>().Await<uint16_t>();

        stream.Read();

        assert(pair1 == buffer[0] + buffer[1]);
        assert(pair2 == buffer[2] + buffer[3]);

        fclose(file);
    }

    // (3) Staggered reading
    {
        char buf1[] = { 'D', 'i', 'e', 't', 'r', 'i', 'c', 'h' };
        char buf2[] = " Buxtehude";

        FILE* file = fmemopen(buf1, sizeof(buf1), "r");
        Stream stream(file);

        std::string str;
        stream.Await(sizeof(buf1) + sizeof(buf2)).Then([&str] (Stream& s, Field& f) {
            auto [data, size] = s[0].GetPtr<char>();
            str.append(data, size);
        });

        assert(stream.Read() == false);
        assert(stream.Status() == buxtehude::StreamStatus::REACHED_EOF);

        fclose(file);

        file = fmemopen(buf2, sizeof(buf2), "r");
        assert(stream.Read() == true);
        assert(stream.Status() == buxtehude::StreamStatus::OKAY);

        assert(strcmp("Dietrich Buxtehude", str.c_str()) == 0);

        fclose(file);
    }

    // (4) Resetting
    {
        int numbers[] = { 1, 2, 3, 4, 5, 6, 7 };

        FILE* file = fmemopen(numbers, sizeof(numbers), "r");

        Stream stream(file);

        int sum = 0;
        stream.Await<int>().Then([&sum] (Stream& s, Field& f) {
            sum += f.Get<int>();
            s.Reset();
        });

        stream.Read();

        assert(sum == 28);

        fclose(file);
    }

    printf("Test (%s) completed successfully\n", __FILE__);

    return 0;
}
