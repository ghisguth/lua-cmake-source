#include <iostream>
#include <cstdlib>

int main(int argc, char** argv)
{
	std::srand(time(0));
	try
	{
	}
	catch(std::exception & e)
	{
		std::cerr<<"unhandled std::exception "<<e.what()<<"\n";
		std::abort();
	}
	catch(...)
	{
		std::cerr<<"unhandled exception\n";
		std::abort();
	}
	return 0;
}

