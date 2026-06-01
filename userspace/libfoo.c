/* A trivial shared library: the dynamic linker must load it and resolve foo()
 * across the DSO boundary (load-time link or dlopen). */
int foo(int x)
{
	return x * 2 + 1;
}
