/*
 * Runs HiFi XI's routine for the game's weather (src/game/gamerain.cpp, gr_code; begun as the worldfx Ashita addon's
 * /worldfx gamerain routine, from reshade-worldfx's tests/gamerain_cave.c) as the game would: 32-bit, called in place
 * of FFXiMain 046070's "mov al, [ebp+17Ah]" with ebp = an effect element, al its answer (non-zero: not drawn), every
 * other register as it was. tests/gamerain_test.cpp writes the routine's memory as gamerain.cpp left it (the whole
 * list filled, rain's and snow's, the all word 0) to a file and runs this:
 *
 *     gamerain_cave.exe <image file>
 *
 * The file: u32 the routine's memory address, u32 size, then that many bytes. The elements, generators, folders
 * and handles here are laid out as the game's (client-notes "Hiding one weather's visuals"): element +0x17A
 * hidden, +0xFC its generator's handle, +0x10C its flags; generator +0x20 name, +0x24 type, +0x28 its parent's
 * handle, +0xDE bit 7 a weather's, +0xE4 its model command (01, +0xC the model's name); folder +0x20 name,
 * +0x24 type 1. The list mode's cases run first (the all word 0, as written), then all mode's (the all word set
 * here, as gamerain.cpp sets it while World FX draws all of the weather), then the list mode's again with it 0.
 * Prints ok/FAIL lines; exit code = failures.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failed = 0;
static void check(int ok, const char *what)
{
	printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
	if (!ok)
		failed++;
}

static DWORD name_of(const char *s)
{
	DWORD v;
	memcpy(&v, s, 4);
	return v;
}

// Fake game objects (zeroed, bigger than the game's: only the fields above are read)
typedef struct { unsigned char b[0x200]; DWORD handle; } object;

static object *make(DWORD type, const char *name)
{
	object *o = (object *)calloc(1, sizeof(object));
	o->handle = (DWORD)o; // (a handle points at a word holding the object)
	*(DWORD *)(o->b + 0x24) = type | (0x10u << 7);
	if (name != NULL)
		*(DWORD *)(o->b + 0x20) = name_of(name);
	return o;
}
static void set_parent(object *child, object *parent) { *(DWORD *)(child->b + 0x28) = parent != NULL ? (DWORD)&parent->handle : 0; }
static object *folder(const char *name, object *parent)
{
	object *f = make(1, name);
	set_parent(f, parent);
	return f;
}
// A weather's generator in a folder, its model command (01 ... name at +0xC) kept in its own bytes
static object *generator(object *in, const char *model, int weather)
{
	object *g = make(5, "gen1");
	set_parent(g, in);
	g->b[0xDE] = weather ? 0x83 : 0x00;
	unsigned char *cmd = g->b + 0x140;
	cmd[0] = 1;
	cmd[1] = 0x0C;
	if (model != NULL)
		memcpy(cmd + 0x0C, model, 4);
	*(DWORD *)(g->b + 0xE4) = (DWORD)cmd;
	return g;
}
static object *element(object *gen, DWORD flags)
{
	object *e = make(0, NULL);
	*(DWORD *)(e->b + 0xFC) = gen != NULL ? (DWORD)&gen->handle : 0;
	*(DWORD *)(e->b + 0x10C) = flags;
	return e;
}

// Calls the routine with ebp = the element and known values in the other registers; what came back
static DWORD routine_at, elem_at, r_eax, r_ebx, r_ecx, r_edx, r_esi, r_edi, r_ebp, esp_in, esp_out;
static void run(object *e)
{
	elem_at = (DWORD)e->b;
	__asm {
		pushad
		mov eax, 0x11111111
		mov ebx, 0x22222222
		mov ecx, 0x33333333
		mov edx, 0x44444444
		mov esi, 0x55555555
		mov edi, 0x66666666
		push ebp
		mov ebp, elem_at
		mov esp_in, esp
		call dword ptr [routine_at]
		mov esp_out, esp
		mov r_ebp, ebp
		pop ebp
		mov r_eax, eax
		mov r_ebx, ebx
		mov r_ecx, ecx
		mov r_edx, edx
		mov r_esi, esi
		mov r_edi, edi
		popad
	}
}

// One element, whether it should come back hidden, and what it is
typedef struct { object *e; int hidden; const char *what; } test_case;

// Each case run once: its answer, the count of skipped draws (+1 only for one hidden by the routine, not by its own
// +0x17A) and every register but al kept; the number the routine hid
static int run_cases(const test_case *cases, int n, volatile DWORD *hidden, const char *mode)
{
	char line[300];
	int hits = 0;
	for (int i = 0; i < n; ++i)
	{
		const DWORD before = *hidden;
		run(cases[i].e);
		const int was_hidden = (r_eax & 0xFF) != 0;
		sprintf(line, "%s: %s (al %lu, hidden count +%lu)", mode, cases[i].what, r_eax & 0xFF, *hidden - before);
		const int counted = cases[i].hidden && cases[i].e->b[0x17A] == 0;
		check(was_hidden == cases[i].hidden && (*hidden - before) == (DWORD)(counted ? 1 : 0), line);
		check((r_eax & 0xFFFFFF00) == 0x11111100 && r_ebx == 0x22222222 && r_ecx == 0x33333333 && r_edx == 0x44444444 &&
			r_esi == 0x55555555 && r_edi == 0x66666666 && r_ebp == (DWORD)cases[i].e->b && esp_in == esp_out,
			"  every register but al as it was, the stack balanced");
		hits += counted;
	}
	return hits;
}

// Whether the seen ring holds this weather and model
static int saw(volatile DWORD *seen, const char *weather, const char *model)
{
	for (int i = 0; i < 32; ++i)
		if (seen[i * 2] == name_of(weather) && seen[i * 2 + 1] == name_of(model))
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("usage: gamerain_cave <image file>\n");
		return 1;
	}
	FILE *f = fopen(argv[1], "rb");
	DWORD head[2];
	if (f == NULL || fread(head, 4, 2, f) != 2 || head[1] == 0 || head[1] > 0x10000)
	{
		printf("FAIL  can't read %s\n", argv[1]);
		return 1;
	}
	const DWORD mem = head[0], size = head[1];
	if (VirtualAlloc((void *)(mem & ~0xFFFFu), 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE) == NULL)
	{
		printf("FAIL  can't map 0x%08lX in this process\n", mem);
		return 1;
	}
	if (fread((void *)mem, 1, size, f) != size)
	{
		printf("FAIL  short image\n");
		return 1;
	}
	fclose(f);
	routine_at = mem + 0x190;
	volatile DWORD *hidden = (DWORD *)(mem + 0x00), *seen_at = (DWORD *)(mem + 0x04), *all = (DWORD *)(mem + 0x0C);
	volatile DWORD *list = (DWORD *)(mem + 0x10), *seen = (DWORD *)(mem + 0x90);
	int entries = 0;
	while (entries < 16 && list[entries * 2] != 0)
		entries++;
	printf("the routine at 0x%08lX, %d pairs on its list, its all word %lu\n", mem, entries, *all);
	check(entries >= 4 && list[0] == name_of("rain") && list[1] == 0, "gamerain.cpp filled the list (rain, any model first)");
	check(entries == 16 && list[30] == name_of("bli1") && list[31] == 0, "the whole list: 16 pairs, no end marker (the last: bli1, any model)");
	check(*all == 0, "its all word 0: the list mode");

	object *root = folder("root", NULL);
	set_parent(root, root);
	object *weat = folder("weat", root);
	object *rain = folder("rain", weat), *suny = folder("suny", weat), *bolt = folder("bolt", weat), *squl = folder("squl", weat);
	object *snow = folder("snow", weat), *bliz = folder("bliz", weat), *bli1 = folder("bli1", weat);
	object *effe = folder("effe", root);
	char line[200];

	// The list mode's cases (and the elements all mode runs too)
	test_case cases[32];
	int n = 0;
	object *e;
	object *own_hidden, *no_generator, *dangling, *not_generator, *not_weather, *dome, *clo1, *sun2, *suny_smok, *ligh, *no_model,
		*deep, *outside, *rain_smok, *hit3;
	// (each case's element)
	e = element(NULL, 1); e->b[0x17A] = 1; own_hidden = e;
	cases[n].e = e; cases[n].hidden = 1; cases[n++].what = "an element hidden by the game already: hidden (its +0x17A)";
	no_generator = element(NULL, 1);
	cases[n].e = no_generator; cases[n].hidden = 0; cases[n++].what = "no generator: drawn";
	{
		object *g = make(5, "gen0");
		dangling = element(g, 1);
		g->handle = 0; // (a handle to nothing)
		cases[n].e = dangling; cases[n].hidden = 0; cases[n++].what = "a handle to nothing: drawn";
	}
	{
		object *g = generator(rain, "smok", 1);
		*(DWORD *)(g->b + 0x24) = 3; // (not a generator)
		not_generator = element(g, 1);
		cases[n].e = not_generator; cases[n].hidden = 0; cases[n++].what = "a resource that isn't a generator: drawn";
	}
	not_weather = element(generator(rain, "smok", 0), 1);
	cases[n].e = not_weather; cases[n].hidden = 0; cases[n++].what = "a generator not marked as a weather's: drawn";
	rain_smok = element(generator(rain, "smok", 1), 1);
	cases[n].e = rain_smok; cases[n].hidden = 1; cases[n++].what = "rain's smok: hidden";
	cases[n].e = element(generator(rain, "rain", 1), 0x00000044); cases[n].hidden = 1; cases[n++].what = "rain's rain cylinder (flags 0x44): hidden";
	dome = element(generator(rain, "rain", 1), 0x02440004);
	cases[n].e = dome; cases[n].hidden = 0; cases[n++].what = "rain's cloud dome (flags 0x02440004, a sky layer): drawn";
	cases[n].e = element(generator(squl, "asi1", 1), 0); cases[n].hidden = 1; cases[n++].what = "squall's asi1 rings: hidden";
	suny_smok = element(generator(suny, "smok", 1), 1);
	cases[n].e = suny_smok; cases[n].hidden = 0; cases[n++].what = "sunshine's smok: drawn";
	ligh = element(generator(bolt, "ligh", 1), 0x00000004);
	cases[n].e = ligh; cases[n].hidden = 0; cases[n++].what = "thunderstorm's lightning (ligh): drawn";
	cases[n].e = element(generator(bolt, "smok", 1), 1); cases[n].hidden = 1; cases[n++].what = "thunderstorm's smok: hidden";
	{
		object *g = generator(bolt, "rain", 1);
		((unsigned char *)*(DWORD *)(g->b + 0xE4))[0] = 0x09; // (its first command isn't a model's)
		no_model = element(g, 4);
		cases[n].e = no_model; cases[n].hidden = 0; cases[n++].what = "thunderstorm's generator without a model command: drawn (model unknown)";
	}
	{
		object *source = generator(rain, "smok", 1);
		object *clone = make(5, "clon");
		set_parent(clone, source);
		clone->b[0xDE] = source->b[0xDE];
		*(DWORD *)(clone->b + 0xE4) = *(DWORD *)(source->b + 0xE4);
		cases[n].e = element(clone, 1); cases[n].hidden = 1; cases[n++].what = "a clone of rain's smok: hidden (through its source)";
	}
	{
		object *g = generator(rain, "smok", 1), *top = g;
		for (int i = 0; i < 4; ++i) // (4 clones deep: the folder is a 5th parent away)
		{
			object *c = make(5, "clon");
			set_parent(c, top);
			c->b[0xDE] = 0x83;
			top = c;
		}
		deep = element(top, 1);
		cases[n].e = deep; cases[n].hidden = 0; cases[n++].what = "clones 4 deep: drawn (the routine looks up 4 parents)";
	}
	{
		object *g = generator(effe, "smok", 1);
		outside = element(g, 1);
		cases[n].e = outside; cases[n].hidden = 0; cases[n++].what = "a weather-marked generator outside any weather folder (effe): drawn";
	}
	hit3 = element(generator(snow, "hit3", 1), 1);
	cases[n].e = hit3; cases[n].hidden = 1; cases[n++].what = "snow's hit3 flakes: hidden";
	sun2 = element(generator(snow, "sun2", 1), 0x00400004);
	cases[n].e = sun2; cases[n].hidden = 0; cases[n++].what = "snow's sun2 (flags 0x00400004, a sky layer): drawn";
	clo1 = element(generator(snow, "clo1", 1), 0x00400000);
	cases[n].e = clo1; cases[n].hidden = 0; cases[n++].what = "snow's clo1 clouds (a sky layer): drawn";
	cases[n].e = element(generator(bliz, "smok", 1), 1); cases[n].hidden = 1; cases[n++].what = "blizzard's smok: hidden";
	cases[n].e = element(generator(bli1, "hit3", 1), 1); cases[n].hidden = 1; cases[n++].what = "blizzard's (second name) hit3: hidden (the list's last pair)";
	const int list_cases = n;

	run_cases(cases, list_cases, hidden, "list");

	// The seen ring: the weather elements that got as far as their folder, with their models
	sprintf(line, "the seen ring holds rain/smok and bolt/ligh (%lu seen so far)", *seen_at);
	check(saw(seen, "rain", "smok") && saw(seen, "bolt", "ligh") && *seen_at >= 8, line);

	// Many calls; then an empty list draws everything
	object *many[3] = { element(generator(rain, "smok", 1), 1), element(generator(rain, "rain", 1), 0x02440004), element(generator(suny, "smok", 1), 1) };
	DWORD before = *hidden;
	for (int i = 0; i < 3000; ++i)
		run(many[i % 3]);
	sprintf(line, "3000 calls: %lu hidden (expect 1000)", *hidden - before);
	check(*hidden - before == 1000 && esp_in == esp_out, line);

	// All mode (as gamerain.cpp sets it while World FX draws all of the weather): every weather generator's element
	// skipped, sky layers too, whatever its weather or model, even where its folder isn't found; the rest as before. The
	// list stays filled here (gamerain.cpp empties it), so an element on it too is counted once.
	*all = 1;
	{
		test_case a[32];
		int m = 0;
		object *weather_own_hidden = element(generator(rain, "smok", 1), 1);
		weather_own_hidden->b[0x17A] = 1;
		a[m].e = own_hidden; a[m].hidden = 1; a[m++].what = "an element hidden by the game already: hidden (its own +0x17A)";
		a[m].e = weather_own_hidden; a[m].hidden = 1; a[m++].what = "a weather element hidden by the game already: hidden, not counted (its own +0x17A)";
		{
			object *g = generator(effe, "ee01", 0);
			object *mine = element(g, 1);
			mine->b[0x17A] = 1;
			a[m].e = mine; a[m].hidden = 1; a[m++].what = "a non-weather element with its +0x17A set: hidden still (the game's own)";
		}
		a[m].e = no_generator; a[m].hidden = 0; a[m++].what = "no generator: drawn";
		a[m].e = dangling; a[m].hidden = 0; a[m++].what = "a handle to nothing: drawn";
		a[m].e = not_generator; a[m].hidden = 0; a[m++].what = "a resource that isn't a generator: drawn";
		a[m].e = not_weather; a[m].hidden = 0; a[m++].what = "a generator not marked as a weather's (a non-weather element): drawn";
		a[m].e = element(generator(effe, "l_00", 0), 0x00400000); a[m].hidden = 0; a[m++].what = "a non-weather element with the sky layer flag: drawn";
		a[m].e = rain_smok; a[m].hidden = 1; a[m++].what = "rain's smok (a weather particle, on the list too): hidden, counted once";
		a[m].e = hit3; a[m].hidden = 1; a[m++].what = "snow's hit3 flakes (a weather particle): hidden";
		a[m].e = dome; a[m].hidden = 1; a[m++].what = "rain's cloud dome (flags 0x02440004, a weather sky layer): hidden";
		a[m].e = sun2; a[m].hidden = 1; a[m++].what = "snow's sun2 (flags 0x00400004, a sky layer): hidden";
		a[m].e = clo1; a[m].hidden = 1; a[m++].what = "snow's clo1 clouds (a sky layer): hidden";
		a[m].e = suny_smok; a[m].hidden = 1; a[m++].what = "sunshine's smok (on no list): hidden";
		a[m].e = element(generator(suny, "clod", 1), 0x00400000); a[m].hidden = 1; a[m++].what = "sunshine's clod clouds (a sky layer, on no list): hidden";
		a[m].e = ligh; a[m].hidden = 1; a[m++].what = "thunderstorm's lightning (ligh): hidden";
		a[m].e = element(generator(folder("thdr", weat), "thun", 1), 4); a[m].hidden = 1; a[m++].what = "thunder's thun (on no list): hidden";
		a[m].e = no_model; a[m].hidden = 1; a[m++].what = "thunderstorm's generator without a model command: hidden";
		a[m].e = deep; a[m].hidden = 1; a[m++].what = "clones 4 deep (no folder found): hidden (a weather generator's all the same)";
		a[m].e = outside; a[m].hidden = 1; a[m++].what = "a weather-marked generator outside any weather folder (effe): hidden";

		for (int i = 0; i < 64; ++i)
			seen[i] = 0;
		*seen_at = 0;
		run_cases(a, m, hidden, "all");
		sprintf(line, "all: the seen ring still filled: rain/smok, snow/clo1, suny/smok (%lu seen)", *seen_at);
		check(saw(seen, "rain", "smok") && saw(seen, "snow", "clo1") && saw(seen, "suny", "smok"), line);

		object *many4[4] = { many[0], many[1], many[2], not_weather };
		before = *hidden;
		for (int i = 0; i < 4000; ++i)
			run(many4[i % 4]);
		sprintf(line, "all: 4000 calls, a quarter of them not a weather's: %lu hidden (expect 3000)", *hidden - before);
		check(*hidden - before == 3000 && esp_in == esp_out, line);

		// As gamerain.cpp leaves it in all mode: the list empty
		const DWORD first = list[0];
		list[0] = 0;
		before = *hidden;
		run(dome);
		const DWORD dome_al = r_eax & 0xFF;
		run(rain_smok);
		const DWORD smok_al = r_eax & 0xFF;
		run(not_weather);
		sprintf(line, "all, the list empty: the cloud dome and rain's smok hidden, the non-weather drawn (hidden count +%lu)", *hidden - before);
		check(dome_al != 0 && smok_al != 0 && (r_eax & 0xFF) == 0 && *hidden - before == 2, line);
		list[0] = first;

		// (any word but 0 is all mode: its low byte isn't what's read)
		*all = 0x100;
		run(dome);
		check((r_eax & 0xFF) != 0, "all word 0x100: all mode still, the cloud dome hidden");
	}

	// All mode off again: the list mode exactly as before
	*all = 0;
	run_cases(cases, list_cases, hidden, "list again");

	list[0] = 0;
	run(many[0]);
	check((r_eax & 0xFF) == 0, "an empty list: rain's smok drawn");

	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed;
}
