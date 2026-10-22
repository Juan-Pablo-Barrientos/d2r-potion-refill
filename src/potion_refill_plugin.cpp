#include <D2RLPlugin/console.h>
#include <D2RLPlugin/context.h>
#include <D2RLPlugin/inventory.h>
#include <D2RLPlugin/item.h>
#include <D2RLPlugin/lifecycle_events.h>
#include <D2RLPlugin/threads.h>
#include <D2RLPlugin/version.h>
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// Refills the belt column a potion was drunk from with a matching potion from
// the normal inventory. The refill uses the same two client requests as a manual
// drag (inventory to cursor, cursor to belt slot), so the game server validates
// the move and nothing is created or destroyed.

static constexpr D2RL::PluginInfo PotionRefillPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "potion-refill",
	.name        = "Potion Refill",
	.version     = "0.1.0",
	.author      = "Juan",
	.description = "Refills the belt column you drink from with a matching potion from your inventory.",
	.flags       = D2RL::PluginFlags::Client,
};

static constexpr char DefaultConfig[] = R"(# Potion Refill
# Refills the belt column you drink from with a matching potion from your inventory.

[potion-refill]
# Turn the refill on or off.
enabled = true

# "family": any healing potion refills healing, any mana potion refills mana,
#           any rejuvenation potion refills rejuvenation. The same potion is
#           preferred, then the closest size.
# "exact":  only the same potion, such as Super Healing for Super Healing.
match = "family"

# Write every refill to the plugin log.
log_refills = false
)";

// D2RCore wraps D2R's client-to-server send with this function. It is a private
// export, so its prologue is checked before use and the plugin stays idle when a
// different D2RLoader build changes it.
using SendClientGameplayPacketFn = void(__cdecl*)(const uint8_t* data, uint32_t size) noexcept;

static constexpr uint8_t SendClientGameplayPacketPrologue[] = { 0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x50 };

// D2R client packets, checked against D2Game's handler table in this build.
// 0x19 grew from classic D2's [id] to name the grid cell it picks from.
static constexpr uint8_t PacketCursorToGrid       = 0x18; // [id][x][y][page], 17 bytes
static constexpr uint8_t PacketGridToCursor       = 0x19; // [id][x][y][page], 17 bytes
static constexpr uint8_t PacketCursorToBeltSlot   = 0x23; // [id][slot], 9 bytes
static constexpr uint32_t InventoryGridPage       = 0;

static constexpr int BeltColumns          = 4;
static constexpr int BeltRows             = 4;
static constexpr int BeltSlots            = BeltColumns * BeltRows;
static constexpr int MaxInventoryPotions  = 128;
static constexpr int MaxPendingRefills    = 16;

// The server shifts the column down after a drink. Waiting a few frames lets
// that update arrive before the free slot is chosen.
static constexpr ULONGLONG SettleMs            = 120;
static constexpr ULONGLONG CursorBusyTimeoutMs = 5'000;
static constexpr ULONGLONG PlaceTimeoutMs      = 1'000;
static constexpr ULONGLONG GiveUpMs            = 3'000;

enum class MatchMode : uint32_t {
	Family = 0,
	Exact  = 1,
};

struct Config {
	bool      enabled    = true;
	MatchMode match      = MatchMode::Family;
	bool      logRefills = false;
};

// family is zero for items this plugin does not refill.
struct PotionKind {
	char family = 0;
	int  tier   = 0;
};

struct StoredItem {
	D2RL::ItemHandle handle    = D2RL::InvalidItemHandle;
	uint32_t         runtimeId = 0;
	uint32_t         code      = 0;
	int32_t          x         = 0;
	int32_t          y         = 0;
};

struct Snapshot {
	StoredItem belt[BeltSlots] {};
	bool       beltUsed[BeltSlots] {};
	StoredItem potions[MaxInventoryPotions] {};
	int        potionCount  = 0;
	bool       cursorUsed   = false;
	uint32_t   cursorItemId = 0;
};

enum class RefillStage : uint32_t {
	Waiting = 0,
	Placing = 1,
};

struct RefillRequest {
	RefillStage stage     = RefillStage::Waiting;
	int         column    = 0;
	int         maxRow    = 0;
	uint32_t    code      = 0;
	ULONGLONG   since     = 0;
	uint32_t    runtimeId = 0;
	int32_t     sourceX   = 0;
	int32_t     sourceY   = 0;
	int         slot      = 0;
	bool        returned  = false;
};

static const D2RL::PluginContext*    plugin;
static const D2RL::InventoryService* inventory;
static const D2RL::ItemService*      items;
static const D2RL::ThreadService*    threads;
static SendClientGameplayPacketFn    sendPacket;
static Config                        config;

static bool               loopRunning;
static bool               haveSnapshot;
static D2RL::PlayerHandle snapshotPlayer = D2RL::InvalidPlayerHandle;
static Snapshot           previous;
static RefillRequest      pending[MaxPendingRefills];
static int                pendingCount;

static void Logf(bool warning, const char* format, ...) noexcept {
	char    message[256] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	if (warning) {
		plugin->LogWarn(message);
	} else {
		plugin->LogInfo(message);
	}
}

static auto CodeText(uint32_t code, char (&text)[5]) noexcept -> const char* {
	std::memcpy(text, &code, 4);
	text[4] = 0;
	return text;
}

static auto ClassifyPotion(uint32_t code) noexcept -> PotionKind {
	const char first  = static_cast<char>(code & 0xFFU);
	const char second = static_cast<char>((code >> 8U) & 0xFFU);
	const char third  = static_cast<char>((code >> 16U) & 0xFFU);
	if ((code >> 24U) != ' ') {
		return {};
	}
	if (first == 'h' && second == 'p' && third >= '1' && third <= '5') {
		return { 'h', third - '0' };
	}
	if (first == 'm' && second == 'p' && third >= '1' && third <= '5') {
		return { 'm', third - '0' };
	}
	switch (code) {
	case D2RL::Items::MakeItemCode("rvs"): return { 'r', 1 };
	case D2RL::Items::MakeItemCode("rvl"): return { 'r', 2 };
	case D2RL::Items::MakeItemCode("vps"): return { 's', 1 };
	case D2RL::Items::MakeItemCode("yps"): return { 'a', 1 };
	case D2RL::Items::MakeItemCode("wms"): return { 't', 1 };
	default: return {};
	}
}

// Native belt items store the slot as x. Accept a column/row pair as well.
static auto BeltSlot(const D2RL::Items::ItemInfo* item) noexcept -> int {
	const int slot = item->y > 0 ? item->y * BeltColumns + item->x : item->x;
	return slot >= 0 && slot < BeltSlots ? slot : -1;
}

// ---- Configuration --------------------------------------------------------

static auto TrimLeft(const char* text) noexcept -> const char* {
	while (*text == ' ' || *text == '\t') {
		++text;
	}
	return text;
}

static auto ReadValue(const char* line, const char* key) noexcept -> const char* {
	const size_t length = std::strlen(key);
	if (std::strncmp(line, key, length) != 0) {
		return nullptr;
	}
	const char* rest = TrimLeft(line + length);
	return *rest == '=' ? TrimLeft(rest + 1) : nullptr;
}

static void LoadConfig() noexcept {
	config = {};
	if (!plugin->EnsureConfig(DefaultConfig)) {
		plugin->LogWarn("Could not create the default config. Using defaults.");
		return;
	}

	char text[4'096] {};
	if (!plugin->ReadConfig(text, sizeof(text))) {
		plugin->LogWarn("Could not read the config. Using defaults.");
		return;
	}

	char* context = nullptr;
	for (char* line = strtok_s(text, "\r\n", &context); line != nullptr; line = strtok_s(nullptr, "\r\n", &context)) {
		line = const_cast<char*>(TrimLeft(line));
		if (const char* enabled = ReadValue(line, "enabled")) {
			config.enabled = std::strncmp(enabled, "false", 5) != 0;
		} else if (const char* match = ReadValue(line, "match")) {
			config.match = std::strncmp(match, "\"exact\"", 7) == 0 ? MatchMode::Exact : MatchMode::Family;
		} else if (const char* logRefills = ReadValue(line, "log_refills")) {
			config.logRefills = std::strncmp(logRefills, "true", 4) == 0;
		}
	}
}

// ---- Packets --------------------------------------------------------------

static void SendGridPacket(uint8_t packetId, uint32_t itemId, uint32_t x, uint32_t y) noexcept {
	uint8_t packet[17] { packetId };
	std::memcpy(packet + 1, &itemId, 4);
	std::memcpy(packet + 5, &x, 4);
	std::memcpy(packet + 9, &y, 4);
	std::memcpy(packet + 13, &InventoryGridPage, 4);
	sendPacket(packet, sizeof(packet));
}

static void SendInventoryToCursor(uint32_t itemId, uint32_t x, uint32_t y) noexcept {
	SendGridPacket(PacketGridToCursor, itemId, x, y);
}

static void SendCursorToBeltSlot(uint32_t itemId, uint32_t slot) noexcept {
	uint8_t packet[9] { PacketCursorToBeltSlot };
	std::memcpy(packet + 1, &itemId, 4);
	std::memcpy(packet + 5, &slot, 4);
	sendPacket(packet, sizeof(packet));
}

static void SendCursorToInventory(uint32_t itemId, uint32_t x, uint32_t y) noexcept {
	SendGridPacket(PacketCursorToGrid, itemId, x, y);
}

static auto ResolveSendPacket() noexcept -> SendClientGameplayPacketFn {
	const HMODULE core = GetModuleHandleW(L"D2RCore.dll");
	if (core == nullptr) {
		plugin->LogError("D2RCore.dll is not loaded. Potion Refill is disabled.");
		return nullptr;
	}
	const auto address = reinterpret_cast<const uint8_t*>(GetProcAddress(core, "SendClientGameplayPacket"));
	if (address == nullptr) {
		plugin->LogError("D2RCore.dll has no SendClientGameplayPacket export. Potion Refill is disabled.");
		return nullptr;
	}
	if (std::memcmp(address, SendClientGameplayPacketPrologue, sizeof(SendClientGameplayPacketPrologue)) != 0) {
		plugin->LogError("SendClientGameplayPacket changed in this D2RLoader build. Potion Refill is disabled until it is updated.");
		return nullptr;
	}
	return reinterpret_cast<SendClientGameplayPacketFn>(address);
}

// ---- Snapshots ------------------------------------------------------------

static auto __cdecl CaptureItem(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* userData) noexcept -> D2RL::Inventory::IterationAction {
	auto* snapshot = static_cast<Snapshot*>(userData);
	if (snapshot == nullptr || item == nullptr || item->structSize < D2RL::Items::ItemInfoRequiredSize) {
		return D2RL::Inventory::IterationAction::Stop;
	}

	const StoredItem stored { item->handle, item->runtimeId, item->code, item->x, item->y };
	if (item->container == D2RL::Items::ItemContainer::Belt) {
		const int slot = BeltSlot(item);
		if (slot >= 0) {
			snapshot->belt[slot]     = stored;
			snapshot->beltUsed[slot] = true;
		}
	} else if (item->container == D2RL::Items::ItemContainer::Inventory && ClassifyPotion(item->code).family != 0 && snapshot->potionCount < MaxInventoryPotions) {
		snapshot->potions[snapshot->potionCount++] = stored;
	}
	return D2RL::Inventory::IterationAction::Continue;
}

static auto Capture(D2RL::PlayerHandle player, Snapshot* snapshot) noexcept -> bool {
	const D2RL::Inventory::ItemFilter filter {
		.structSize    = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Belt) | D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory),
	};
	if (inventory->forEachInventoryItem(plugin, player, &filter, CaptureItem, snapshot) != D2RL::Inventory::Result::Success) {
		return false;
	}

	D2RL::ItemHandle cursor = D2RL::InvalidItemHandle;
	if (inventory->getCursorItem(plugin, player, &cursor) == D2RL::Inventory::Result::Success && cursor != D2RL::InvalidItemHandle) {
		D2RL::Items::ItemInfo info { .structSize = D2RL::Items::ItemInfoSize };
		snapshot->cursorUsed = true;
		if (items->getItemInfo(plugin, cursor, &info) == D2RL::Items::Result::Success) {
			snapshot->cursorItemId = info.runtimeId;
		}
	}
	return true;
}

static auto FindInBelt(const Snapshot& snapshot, uint32_t runtimeId) noexcept -> int {
	for (int slot = 0; slot < BeltSlots; ++slot) {
		if (snapshot.beltUsed[slot] && snapshot.belt[slot].runtimeId == runtimeId) {
			return slot;
		}
	}
	return -1;
}

static auto InInventory(const Snapshot& snapshot, uint32_t runtimeId) noexcept -> bool {
	for (int index = 0; index < snapshot.potionCount; ++index) {
		if (snapshot.potions[index].runtimeId == runtimeId) {
			return true;
		}
	}
	return false;
}

static auto OnCursor(const Snapshot& snapshot, uint32_t runtimeId) noexcept -> bool {
	return snapshot.cursorUsed && snapshot.cursorItemId == runtimeId;
}

// ---- Refill logic ---------------------------------------------------------

static void PopRefill() noexcept {
	for (int index = 1; index < pendingCount; ++index) {
		pending[index - 1] = pending[index];
	}
	if (pendingCount > 0) {
		--pendingCount;
	}
}

// A drink removes one belt potion without moving it anywhere the player can see.
// Several items vanishing at once (death, removing the belt) is ignored.
static void DetectDrink(const Snapshot& now, ULONGLONG time) noexcept {
	int vanishedSlot  = -1;
	int vanishedCount = 0;
	for (int slot = 0; slot < BeltSlots; ++slot) {
		if (!previous.beltUsed[slot]) {
			continue;
		}
		const uint32_t id = previous.belt[slot].runtimeId;
		if (FindInBelt(now, id) >= 0 || InInventory(now, id) || OnCursor(now, id)) {
			continue;
		}
		vanishedSlot = slot;
		++vanishedCount;
	}
	if (vanishedCount != 1 || ClassifyPotion(previous.belt[vanishedSlot].code).family == 0 || pendingCount == MaxPendingRefills) {
		return;
	}

	const int column = vanishedSlot % BeltColumns;
	int       maxRow = 0;
	for (int row = 0; row < BeltRows; ++row) {
		if (previous.beltUsed[row * BeltColumns + column]) {
			maxRow = row;
		}
	}
	pending[pendingCount++] = {
		.column = column,
		.maxRow = maxRow,
		.code   = previous.belt[vanishedSlot].code,
		.since  = time,
	};
}

static auto ChooseSource(const Snapshot& now, uint32_t code) noexcept -> const StoredItem* {
	const PotionKind  wanted = ClassifyPotion(code);
	const StoredItem* best   = nullptr;
	int               bestScore = 0;
	for (int index = 0; index < now.potionCount; ++index) {
		const StoredItem& candidate = now.potions[index];
		const PotionKind  kind      = ClassifyPotion(candidate.code);
		if (candidate.code == code) {
			return &candidate;
		}
		if (config.match == MatchMode::Exact || kind.family != wanted.family) {
			continue;
		}
		// Closest size first; on a tie, the larger potion.
		const int distance = kind.tier > wanted.tier ? kind.tier - wanted.tier : wanted.tier - kind.tier;
		const int score    = distance * 2 + (kind.tier < wanted.tier ? 1 : 0);
		if (best == nullptr || score < bestScore) {
			best      = &candidate;
			bestScore = score;
		}
	}
	return best;
}

static void StartRefill(RefillRequest& request, const Snapshot& now, ULONGLONG time) noexcept {
	if (time - request.since < SettleMs) {
		return;
	}
	if (now.cursorUsed) {
		if (time - request.since > CursorBusyTimeoutMs) {
			PopRefill();
		}
		return;
	}

	int slot = -1;
	for (int row = 0; row <= request.maxRow; ++row) {
		if (!now.beltUsed[row * BeltColumns + request.column]) {
			slot = row * BeltColumns + request.column;
			break;
		}
	}
	const StoredItem* source = slot >= 0 ? ChooseSource(now, request.code) : nullptr;
	if (source == nullptr) {
		if (config.logRefills) {
			char code[5] {};
			Logf(false, "No refill for belt column %d (%s): %s.", request.column + 1, CodeText(request.code, code), slot < 0 ? "column is already full" : "no matching potion in the inventory");
		}
		PopRefill();
		return;
	}

	if (config.logRefills) {
		char code[5] {};
		Logf(false, "Refilling belt column %d, slot %d, with %s id=%u from inventory cell %d,%d.", request.column + 1, slot, CodeText(source->code, code), source->runtimeId, source->x, source->y);
	}
	request.stage     = RefillStage::Placing;
	request.runtimeId = source->runtimeId;
	request.sourceX   = source->x;
	request.sourceY   = source->y;
	request.slot      = slot;
	request.since     = time;
	SendInventoryToCursor(source->runtimeId, static_cast<uint32_t>(source->x), static_cast<uint32_t>(source->y));
	SendCursorToBeltSlot(source->runtimeId, static_cast<uint32_t>(slot));
}

static void FinishRefill(RefillRequest& request, const Snapshot& now, ULONGLONG time) noexcept {
	const int slot = FindInBelt(now, request.runtimeId);
	if (slot >= 0) {
		if (config.logRefills) {
			char code[5] {};
			Logf(false, "Refilled belt column %d, slot %d, with %s.", request.column + 1, slot, CodeText(now.belt[slot].code, code));
		}
		PopRefill();
		return;
	}

	// The belt rejected the potion. Put it back where it came from.
	if (OnCursor(now, request.runtimeId) && !request.returned && time - request.since > PlaceTimeoutMs) {
		Logf(true, "Belt slot %d rejected the refill. Returning the potion to the inventory.", request.slot);
		request.returned = true;
		request.since    = time;
		SendCursorToInventory(request.runtimeId, static_cast<uint32_t>(request.sourceX), static_cast<uint32_t>(request.sourceY));
		return;
	}
	if (time - request.since > GiveUpMs) {
		if (OnCursor(now, request.runtimeId)) {
			plugin->LogWarn("A refill potion is still on the cursor. Place it by hand.");
		}
		PopRefill();
	}
}

static void Poll() noexcept {
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (inventory->getLocalPlayer(plugin, &player) != D2RL::Inventory::Result::Success || player == D2RL::InvalidPlayerHandle) {
		haveSnapshot = false;
		pendingCount = 0;
		return;
	}
	if (player != snapshotPlayer) {
		haveSnapshot   = false;
		pendingCount   = 0;
		snapshotPlayer = player;
	}

	Snapshot now;
	if (!Capture(player, &now)) {
		return;
	}

	const ULONGLONG time = GetTickCount64();
	if (haveSnapshot) {
		DetectDrink(now, time);
		if (pendingCount > 0) {
			RefillRequest& request = pending[0];
			if (request.stage == RefillStage::Waiting) {
				StartRefill(request, now, time);
			} else {
				FinishRefill(request, now, time);
			}
		}
	}
	previous     = now;
	haveSnapshot = true;
}

static void __cdecl Tick(const D2RL::PluginContext* context, void*) noexcept {
	if (config.enabled && sendPacket != nullptr) {
		Poll();
	}
	loopRunning = threads->runOnUiThread(context, Tick, nullptr) == D2RL::Threads::Result::Success;
}

static void StartLoop() noexcept {
	if (!loopRunning) {
		loopRunning = threads->runOnUiThread(plugin, Tick, nullptr) == D2RL::Threads::Result::Success;
	}
}

static void __cdecl OnLocalPlayerReady(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent*, void*) noexcept {
	StartLoop();
}

// ---- Diagnostics ----------------------------------------------------------

static void __cdecl DumpBelt(const D2RL::PluginContext* context, void*) noexcept {
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	Snapshot           now;
	if (inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success || !Capture(player, &now)) {
		context->WriteConsoleMessage("potion-refill: no local player.");
		return;
	}

	char line[160] {};
	char code[5] {};
	for (int slot = 0; slot < BeltSlots; ++slot) {
		if (now.beltUsed[slot]) {
			std::snprintf(line, sizeof(line), "belt slot %2d: %s id=%u x=%d y=%d", slot, CodeText(now.belt[slot].code, code), now.belt[slot].runtimeId, now.belt[slot].x, now.belt[slot].y);
			context->WriteConsoleMessage(line);
		}
	}
	for (int index = 0; index < now.potionCount; ++index) {
		const StoredItem& potion = now.potions[index];
		std::snprintf(line, sizeof(line), "inventory: %s id=%u x=%d y=%d", CodeText(potion.code, code), potion.runtimeId, potion.x, potion.y);
		context->WriteConsoleMessage(line);
	}
	std::snprintf(line, sizeof(line), "send=%s enabled=%s match=%s", sendPacket != nullptr ? "ok" : "missing", config.enabled ? "true" : "false", config.match == MatchMode::Exact ? "exact" : "family");
	context->WriteConsoleMessage(line);
}

static auto DumpCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	return threads->runOnUiThread(command->plugin, DumpBelt, nullptr) == D2RL::Threads::Result::Success ? D2RL::ConsoleCommandResult::Handled : D2RL::ConsoleCommandResult::Failed;
}

// ---- Plugin entry points --------------------------------------------------

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &PotionRefillPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	plugin = context;

	if (context->QueryService(&inventory) != D2RL::ServiceQueryResult::Success || !D2RL::HasInventoryServiceField(inventory, D2RL::InventoryServiceRequiredSize)) {
		context->LogError("The inventory service is unavailable.");
		return false;
	}
	if (context->QueryService(&items) != D2RL::ServiceQueryResult::Success || !D2RL::HasItemServiceField(items, D2RL::ItemServiceRequiredSize)) {
		context->LogError("The item service is unavailable.");
		return false;
	}
	if (context->QueryService(&threads) != D2RL::ServiceQueryResult::Success || !D2RL::HasThreadServiceField(threads, D2RL::ThreadServiceRequiredSize)) {
		context->LogError("The thread service is unavailable.");
		return false;
	}

	LoadConfig();
	sendPacket = ResolveSendPacket();

	const D2RL::LifecycleService* lifecycle = nullptr;
	if (context->QueryService(&lifecycle) == D2RL::ServiceQueryResult::Success && D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize)) {
		const D2RL::Lifecycle::GameplayEventListener listener {
			.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
			.kind       = D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady,
			.callback   = OnLocalPlayerReady,
		};
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		(void)lifecycle->registerGameplayEventListener(context, &listener, &handle);
	}
	StartLoop();

	(void)context->RegisterConsoleCommand("potion-refill-dump", DumpCommand, "List belt and inventory potions as Potion Refill sees them.");
	Logf(false, "Potion Refill loaded: enabled=%s match=%s send=%s.", config.enabled ? "true" : "false", config.match == MatchMode::Exact ? "exact" : "family", sendPacket != nullptr ? "ok" : "missing");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	loopRunning  = false;
	haveSnapshot = false;
	pendingCount = 0;
}
