#include "common.hpp"
#include "overlay/d3d12_hook.hpp"
#include "overlay/menu.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <vector>
#include <mutex>

// Provided by imgui_impl_win32.cpp — translates Win32 messages into ImGui input.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Client::Overlay {
	// --- original trampolines ---
	using PresentFn = HRESULT(*)(IDXGISwapChain3*, UINT, UINT);
	using ResizeBuffersFn = HRESULT(*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
	using ExecuteCommandListsFn = void(*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

	static PresentFn oPresent = nullptr;
	static ResizeBuffersFn oResizeBuffers = nullptr;
	static ExecuteCommandListsFn oExecuteCommandLists = nullptr;

	// --- captured state ---
	static ID3D12Device* g_Device = nullptr;
	static ID3D12CommandQueue* g_CommandQueue = nullptr;
	static HWND g_Window = nullptr;
	static WNDPROC oWndProc = nullptr;                 // game's original window procedure
	static std::atomic_bool g_Captured{ false };
	static std::atomic_bool g_MenuOpen{ false };       // toggled by Insert; gates input capture + menu draw
	static std::atomic_uint64_t g_FrameCount{ 0 };

	// --- renderer state (built once from the game's swapchain) ---
	struct FrameContext {
		ID3D12CommandAllocator* commandAllocator = nullptr;
		ID3D12Resource* backBuffer = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
	};

	static bool g_RendererReady = false;
	static bool g_RendererFailed = false;      // gave up after an init failure; don't retry every frame
	static ID3D12DescriptorHeap* g_RtvHeap = nullptr;
	static ID3D12DescriptorHeap* g_SrvHeap = nullptr;   // single descriptor for the ImGui font
	static ID3D12GraphicsCommandList* g_CommandList = nullptr;
	static std::vector<FrameContext> g_Frames;

	// Identity of the swapchain our render targets were built from, plus the shape we built for.
	// If any of this changes under us (resolution/display/fullscreen change, or the game builds a
	// brand new swapchain) our RTVs are stale and must be rebuilt before we draw again.
	static IDXGISwapChain3* g_SwapChain = nullptr;      // non-owning: identity only, never dereferenced blind
	static UINT g_ScWidth = 0, g_ScHeight = 0, g_ScBufferCount = 0;
	static DXGI_FORMAT g_ScFormat = DXGI_FORMAT_UNKNOWN;

	// Present (render thread) and ResizeBuffers (usually the game/main thread) both touch the
	// renderer state, so every path that builds or destroys it takes this.
	static std::recursive_mutex g_RendererMutex;

	// GPU sync: we must not release back buffers / command lists while the GPU is still reading them.
	static ID3D12Fence* g_Fence = nullptr;
	static HANDLE g_FenceEvent = nullptr;
	static std::uint64_t g_FenceValue = 0;

	// Block until the command queue has drained everything we submitted.
	static void WaitForGpu() {
		if (!g_Fence || !g_FenceEvent || !g_CommandQueue) {
			return;
		}
		const std::uint64_t target = ++g_FenceValue;
		if (FAILED(g_CommandQueue->Signal(g_Fence, target))) {
			return;
		}
		if (g_Fence->GetCompletedValue() < target) {
			if (SUCCEEDED(g_Fence->SetEventOnCompletion(target, g_FenceEvent))) {
				WaitForSingleObject(g_FenceEvent, 1000); // bounded: never hang the render thread
			}
		}
	}

	// Tears down the swapchain-dependent objects (RTVs, back buffers, command list/allocators).
	// Keeps the ImGui context + backends alive. Called on resize before rebuilding.
	static void ReleaseRenderTargets() {
		if (g_CommandList) { g_CommandList->Release(); g_CommandList = nullptr; }
		for (auto& f : g_Frames) {
			if (f.commandAllocator) f.commandAllocator->Release();
			if (f.backBuffer) f.backBuffer->Release();
		}
		g_Frames.clear();
		if (g_RtvHeap) { g_RtvHeap->Release(); g_RtvHeap = nullptr; }
	}

	// Full renderer teardown: drains the GPU, drops the DX12 ImGui backend and every reference we
	// hold to the swapchain's buffers. MUST run before the game's ResizeBuffers call — DXGI fails
	// that call outright if anyone still holds a back-buffer reference, and the game does not
	// survive the failure. Deliberately keeps the ImGui context, the Win32 backend and our WndProc
	// subclass alive, so the menu state and input hook survive a mode change.
	static void TeardownRenderer() {
		std::lock_guard<std::recursive_mutex> lock(g_RendererMutex);

		WaitForGpu();
		// Only shut the DX12 backend down if it actually came up (a half-finished InitRenderer
		// lands here too, and ImGui asserts on shutting down a backend that was never inited).
		if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().BackendRendererUserData != nullptr) {
			ImGui_ImplDX12_Shutdown();
		}
		ReleaseRenderTargets();
		if (g_SrvHeap) { g_SrvHeap->Release(); g_SrvHeap = nullptr; }
		if (g_Fence) { g_Fence->Release(); g_Fence = nullptr; }
		if (g_FenceEvent) { CloseHandle(g_FenceEvent); g_FenceEvent = nullptr; }

		g_RendererReady = false;
		g_SwapChain = nullptr;
		g_ScWidth = g_ScHeight = g_ScBufferCount = 0;
		g_ScFormat = DXGI_FORMAT_UNKNOWN;
	}

	// Is this a keyboard/mouse input message we want to keep away from the game while the menu is open?
	static bool IsInputMessage(UINT msg) {
		return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||   // WM_MOUSEMOVE .. WM_MOUSEHWHEEL
			msg == WM_MOUSEHWHEEL ||
			(msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||          // WM_KEYDOWN/UP/CHAR/SYSKEY*
			msg == WM_SETCURSOR;
	}

	// Our window procedure. Toggles the menu on Insert, feeds input to ImGui, and — while the
	// menu is open — swallows input so it doesn't also reach the game.
	static LRESULT WINAPI hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		if (msg == WM_KEYUP && wParam == VK_INSERT) {
			g_MenuOpen = !g_MenuOpen;
			LOG("Overlay", INFO, "Menu {}", g_MenuOpen.load() ? "opened" : "closed");
		}

		if (g_MenuOpen.load()) {
			ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
			if (IsInputMessage(msg)) {
				return TRUE; // consume: keep mouse/keyboard out of the game while the menu has focus
			}
		}

		return CallWindowProcW(oWndProc, hWnd, msg, wParam, lParam);
	}

	// Build everything ImGui's DX12 backend needs from the captured device + the live swapchain.
	static bool InitRenderer(IDXGISwapChain3* swapChain) {
		DXGI_SWAP_CHAIN_DESC desc{};
		if (FAILED(swapChain->GetDesc(&desc))) {
			LOG("Overlay", ERROR, "InitRenderer: swapChain->GetDesc failed.");
			return false;
		}
		const UINT bufferCount = desc.BufferCount;
		const DXGI_FORMAT format = desc.BufferDesc.Format;
		g_Window = desc.OutputWindow;

		// Remember what we are building for, so Present can notice when it stops matching.
		g_SwapChain = swapChain;
		g_ScWidth = desc.BufferDesc.Width;
		g_ScHeight = desc.BufferDesc.Height;
		g_ScBufferCount = bufferCount;
		g_ScFormat = format;

		if (FAILED(g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fence)))) {
			LOG("Overlay", ERROR, "InitRenderer: create fence failed.");
			return false;
		}
		g_FenceValue = 0;
		g_FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (g_FenceEvent == nullptr) {
			LOG("Overlay", ERROR, "InitRenderer: create fence event failed.");
			return false;
		}

		// SRV heap: one shader-visible descriptor for the font texture.
		D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
		srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvDesc.NumDescriptors = 1;
		srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(g_Device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_SrvHeap)))) {
			LOG("Overlay", ERROR, "InitRenderer: create SRV heap failed.");
			return false;
		}

		// RTV heap: one descriptor per back buffer.
		D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
		rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvDesc.NumDescriptors = bufferCount;
		rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if (FAILED(g_Device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_RtvHeap)))) {
			LOG("Overlay", ERROR, "InitRenderer: create RTV heap failed.");
			return false;
		}

		const UINT rtvStep = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();

		g_Frames.resize(bufferCount);
		for (UINT i = 0; i < bufferCount; ++i) {
			FrameContext& f = g_Frames[i];
			f.rtvHandle = rtvHandle;
			if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&f.backBuffer)))) {
				LOG("Overlay", ERROR, "InitRenderer: GetBuffer({}) failed.", i);
				return false;
			}
			g_Device->CreateRenderTargetView(f.backBuffer, nullptr, rtvHandle);
			if (FAILED(g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.commandAllocator)))) {
				LOG("Overlay", ERROR, "InitRenderer: create command allocator {} failed.", i);
				return false;
			}
			rtvHandle.ptr += rtvStep;
		}

		if (FAILED(g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			g_Frames[0].commandAllocator, nullptr, IID_PPV_ARGS(&g_CommandList)))) {
			LOG("Overlay", ERROR, "InitRenderer: create command list failed.");
			return false;
		}
		g_CommandList->Close(); // we Reset() it each frame

		// ImGui context + backends.
		IMGUI_CHECKVERSION();
		if (ImGui::GetCurrentContext() == nullptr) {
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr; // don't write imgui.ini into the game folder
			ImGui::StyleColorsDark();
			ImGui_ImplWin32_Init(g_Window);

			// Subclass the game's window so we see its input messages.
			oWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
				g_Window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hkWndProc)));
			LOG("Overlay", INFO, "Window subclassed - original WndProc @ 0x{:016X}. Press INSERT to toggle the menu.",
				PTR_AS(std::uintptr_t, oWndProc));
		}

		ImGui_ImplDX12_Init(g_Device, bufferCount, format, g_SrvHeap,
			g_SrvHeap->GetCPUDescriptorHandleForHeapStart(),
			g_SrvHeap->GetGPUDescriptorHandleForHeapStart());

		LOG("Overlay", INFO, "Renderer ready - buffers={} format={} hwnd=0x{:016X}",
			bufferCount, static_cast<int>(format), PTR_AS(std::uintptr_t, g_Window));
		return true;
	}

	static void RenderFrame(IDXGISwapChain3* swapChain) {
		const UINT idx = swapChain->GetCurrentBackBufferIndex();
		if (idx >= g_Frames.size()) {
			return; // swapchain grew under us; the next Present rebuilds
		}

		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		ImGui::GetIO().MouseDrawCursor = g_MenuOpen.load(); // draw a software cursor only while the menu is up

		Menu::RenderScriptMessages();
		if (g_MenuOpen.load()) {
			Menu::Render();
		}

		ImGui::Render();

		FrameContext& f = g_Frames[idx];

		f.commandAllocator->Reset();
		g_CommandList->Reset(f.commandAllocator, nullptr);

		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = f.backBuffer;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		g_CommandList->ResourceBarrier(1, &barrier);

		g_CommandList->OMSetRenderTargets(1, &f.rtvHandle, FALSE, nullptr);
		g_CommandList->SetDescriptorHeaps(1, &g_SrvHeap);
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_CommandList);

		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		g_CommandList->ResourceBarrier(1, &barrier);

		g_CommandList->Close();
		ID3D12CommandList* lists[] = { g_CommandList };
		g_CommandQueue->ExecuteCommandLists(1, lists);
	}

	// The game submits its frames on a DIRECT command queue. ImGui's DX12 backend needs
	// that queue to execute our overlay command list, so grab the first DIRECT one we see.
	static void hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT numLists, ID3D12CommandList* const* lists) {
		if (!g_CommandQueue && queue != nullptr) {
			D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
			if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
				g_CommandQueue = queue;
			}
		}
		oExecuteCommandLists(queue, numLists, lists);
	}

	// The game resizes its swapchain on any resolution / display / fullscreen change, and Windows
	// also drives one when the window is dragged to a display with different properties. DXGI
	// rejects the call while *anyone* holds a back-buffer reference — including us — so drop
	// everything first and let Present rebuild on the next frame.
	static HRESULT WINAPI hkResizeBuffers(IDXGISwapChain3* swapChain, UINT bufferCount, UINT width,
		UINT height, DXGI_FORMAT newFormat, UINT flags) {
		{
			std::lock_guard<std::recursive_mutex> lock(g_RendererMutex);
			if (g_RendererReady && swapChain == g_SwapChain) {
				LOG("Overlay", INFO, "ResizeBuffers({}x{}, buffers={}) - releasing overlay render targets.",
					width, height, bufferCount);
				TeardownRenderer();
			}
		}
		return oResizeBuffers(swapChain, bufferCount, width, height, newFormat, flags);
	}

	// True when the chain we built our RTVs from is no longer the chain being presented, or it has
	// changed shape behind our back (some paths recreate the swapchain outright instead of resizing).
	static bool SwapChainChanged(IDXGISwapChain3* swapChain) {
		if (swapChain != g_SwapChain) {
			return true;
		}
		DXGI_SWAP_CHAIN_DESC desc{};
		if (FAILED(swapChain->GetDesc(&desc))) {
			return true;
		}
		return desc.BufferDesc.Width != g_ScWidth
			|| desc.BufferDesc.Height != g_ScHeight
			|| desc.BufferCount != g_ScBufferCount
			|| desc.BufferDesc.Format != g_ScFormat;
	}

	static HRESULT hkPresent(IDXGISwapChain3* swapChain, UINT syncInterval, UINT flags) {
		++g_FrameCount;

		std::lock_guard<std::recursive_mutex> lock(g_RendererMutex);

		if (!g_Captured && g_CommandQueue != nullptr) {
			ID3D12Device* device = nullptr;
			if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr) {
				g_Device = device; // keep the ref; released on teardown
				g_Captured = true;

				DXGI_SWAP_CHAIN_DESC scDesc{};
				swapChain->GetDesc(&scDesc);
				LOG("Overlay", INFO, "D3D12 captured - device=0x{:016X} queue=0x{:016X} swapchain=0x{:016X} buffers={} hwnd=0x{:016X}",
					PTR_AS(std::uintptr_t, g_Device), PTR_AS(std::uintptr_t, g_CommandQueue),
					PTR_AS(std::uintptr_t, swapChain), scDesc.BufferCount, PTR_AS(std::uintptr_t, scDesc.OutputWindow));
			}
		}

		// Rebuild if the swapchain we were drawing into is gone or has changed shape. This covers
		// the paths that never go through ResizeBuffers (e.g. a swapchain recreated from scratch).
		if (g_RendererReady && SwapChainChanged(swapChain)) {
			LOG("Overlay", INFO, "Swapchain changed - rebuilding overlay render targets.");
			TeardownRenderer();
		}

		if (g_Captured && !g_RendererReady && !g_RendererFailed) {
			if (InitRenderer(swapChain)) {
				g_RendererReady = true;
			} else {
				g_RendererFailed = true;
				TeardownRenderer();
				LOG("Overlay", ERROR, "Renderer init failed - overlay disabled for this session.");
			}
		}

		if (g_RendererReady) {
			static bool loggedFirstDraw = false;
			if (!loggedFirstDraw) {
				LOG("Overlay", INFO, "Overlay drawing - ImGui demo window active.");
				loggedFirstDraw = true;
			}
			RenderFrame(swapChain);
		}

		return oPresent(swapChain, syncInterval, flags);
	}

	// Spin up a throwaway device + swapchain purely to read the vtable slots we need to hook.
	// The vtables are process-wide (same d3d12.dll / dxgi.dll), so the addresses match the
	// game's real objects.
	bool Initialize() {
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"cwmod_dummy_wnd";
		RegisterClassExW(&wc);
		HWND hwnd = CreateWindowW(wc.lpszClassName, L"cwmod", WS_OVERLAPPEDWINDOW,
			0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
		if (hwnd == nullptr) {
			LOG("Overlay", ERROR, "Failed to create dummy window for vtable resolution.");
			UnregisterClassW(wc.lpszClassName, wc.hInstance);
			return false;
		}

		auto cleanup = [&](ID3D12Device* dev, ID3D12CommandQueue* q, IDXGIFactory4* f, IDXGISwapChain1* sc) {
			if (sc) sc->Release();
			if (f) f->Release();
			if (q) q->Release();
			if (dev) dev->Release();
			DestroyWindow(hwnd);
			UnregisterClassW(wc.lpszClassName, wc.hInstance);
		};

		ID3D12Device* dummyDevice = nullptr;
		if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDevice)))) {
			LOG("Overlay", ERROR, "D3D12CreateDevice failed.");
			cleanup(nullptr, nullptr, nullptr, nullptr);
			return false;
		}

		D3D12_COMMAND_QUEUE_DESC qDesc{};
		qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		ID3D12CommandQueue* dummyQueue = nullptr;
		if (FAILED(dummyDevice->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&dummyQueue)))) {
			LOG("Overlay", ERROR, "CreateCommandQueue failed.");
			cleanup(dummyDevice, nullptr, nullptr, nullptr);
			return false;
		}

		IDXGIFactory4* factory = nullptr;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
			LOG("Overlay", ERROR, "CreateDXGIFactory1 failed.");
			cleanup(dummyDevice, dummyQueue, nullptr, nullptr);
			return false;
		}

		DXGI_SWAP_CHAIN_DESC1 scDesc{};
		scDesc.BufferCount = 2;
		scDesc.Width = 100;
		scDesc.Height = 100;
		scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scDesc.SampleDesc.Count = 1;
		scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		IDXGISwapChain1* dummySwapChain = nullptr;
		if (FAILED(factory->CreateSwapChainForHwnd(dummyQueue, hwnd, &scDesc, nullptr, nullptr, &dummySwapChain))) {
			LOG("Overlay", ERROR, "CreateSwapChainForHwnd failed.");
			cleanup(dummyDevice, dummyQueue, factory, nullptr);
			return false;
		}

		// Resolve targets from the vtables.
		void* presentTarget = VT_GET(dummySwapChain, 8);          // IDXGISwapChain::Present
		void* resizeTarget = VT_GET(dummySwapChain, 13);          // IDXGISwapChain::ResizeBuffers
		void* executeTarget = VT_GET(dummyQueue, 10);             // ID3D12CommandQueue::ExecuteCommandLists

		cleanup(dummyDevice, dummyQueue, factory, dummySwapChain);

		bool ok = true;
		if (MH_CreateHook(presentTarget, &hkPresent, reinterpret_cast<void**>(&oPresent)) != MH_OK ||
			MH_EnableHook(presentTarget) != MH_OK) {
			LOG("Overlay", ERROR, "Failed to hook IDXGISwapChain::Present @ 0x{:016X}", PTR_AS(std::uintptr_t, presentTarget));
			ok = false;
		}
		if (MH_CreateHook(resizeTarget, &hkResizeBuffers, reinterpret_cast<void**>(&oResizeBuffers)) != MH_OK ||
			MH_EnableHook(resizeTarget) != MH_OK) {
			LOG("Overlay", ERROR, "Failed to hook IDXGISwapChain::ResizeBuffers @ 0x{:016X}", PTR_AS(std::uintptr_t, resizeTarget));
			ok = false;
		}
		if (MH_CreateHook(executeTarget, &hkExecuteCommandLists, reinterpret_cast<void**>(&oExecuteCommandLists)) != MH_OK ||
			MH_EnableHook(executeTarget) != MH_OK) {
			LOG("Overlay", ERROR, "Failed to hook ID3D12CommandQueue::ExecuteCommandLists @ 0x{:016X}", PTR_AS(std::uintptr_t, executeTarget));
			ok = false;
		}

		if (ok) {
			LOG("Overlay", INFO, "D3D12 hooks installed - Present @ 0x{:016X}, ResizeBuffers @ 0x{:016X}, ExecuteCommandLists @ 0x{:016X}",
				PTR_AS(std::uintptr_t, presentTarget), PTR_AS(std::uintptr_t, resizeTarget),
				PTR_AS(std::uintptr_t, executeTarget));
		}
		return ok;
	}

	std::uint64_t FrameCount() {
		return g_FrameCount.load();
	}

	bool IsMenuOpen() {
		return g_MenuOpen.load();
	}
}
