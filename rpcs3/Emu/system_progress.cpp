#include "stdafx.h"
#include "system_progress.hpp"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/overlay_message_dialog.h"
#include "Emu/RSX/Overlays/overlay_message.h"
#include "Emu/RSX/Overlays/overlay_compile_notification.h"
#include "Emu/System.h"

LOG_CHANNEL(sys_log, "SYS");

// Progress display server synchronization variables
atomic_t<const char*> g_progr{nullptr};
atomic_t<u32> g_progr_ftotal{0};
atomic_t<u32> g_progr_fdone{0};
atomic_t<u32> g_progr_ptotal{0};
atomic_t<u32> g_progr_pdone{0};

// For Batch PPU Compilation
atomic_t<bool> g_system_progress_canceled{false};

// For showing feedback while stopping emulation
atomic_t<bool> g_system_progress_stopping{false};

namespace rsx::overlays
{
	class progress_dialog : public message_dialog
	{
	public:
		using message_dialog::message_dialog;
	};
} // namespace rsx::overlays

void progress_dialog_server::operator()()
{
	std::shared_ptr<rsx::overlays::progress_dialog> native_dlg;
	g_system_progress_stopping = false;

	while (!g_system_progress_stopping && thread_ctrl::state() != thread_state::aborting)
	{
		// Wait for the start condition
		auto text0 = +g_progr;

		while (!text0)
		{
			if (g_system_progress_stopping || thread_ctrl::state() == thread_state::aborting)
			{
				break;
			}

			thread_ctrl::wait_for(5000);
			text0 = +g_progr;
		}

		if (g_system_progress_stopping || thread_ctrl::state() == thread_state::aborting)
		{
			break;
		}

		g_system_progress_canceled = false;

		// Initialize message dialog
		bool show_overlay_message = false; // Only show an overlay message after initial loading is done.
		std::shared_ptr<MsgDialogBase> dlg;

		if (const auto renderer = rsx::get_current_renderer())
		{
			// Some backends like OpenGL actually initialize a lot of driver objects in the "on_init" method.
			// Wait for init to complete within reasonable time. Abort just in case we have hardware/driver issues.
			renderer->is_initialized.wait(false, atomic_wait_timeout(5 * 1000000000ull));

			auto manager  = g_fxo->try_get<rsx::overlays::display_manager>();
			show_overlay_message = g_fxo->get<progress_dialog_workaround>().show_overlay_message_only;

			if (manager && !show_overlay_message)
			{
				MsgDialogType type{};
				type.se_mute_on         = true;
				type.se_normal          = true;
				type.bg_invisible       = true;
				type.disable_cancel     = true;
				type.progress_bar_count = 1;

				native_dlg = manager->create<rsx::overlays::progress_dialog>(true);
				native_dlg->show(false, text0, type, nullptr);
				native_dlg->progress_bar_set_message(0, "Please wait");
			}
		}

		if (!show_overlay_message && !native_dlg && (dlg = Emu.GetCallbacks().get_msg_dialog()))
		{
			dlg->type.se_normal          = true;
			dlg->type.bg_invisible       = true;
			dlg->type.progress_bar_count = 1;
			dlg->on_close = [](s32 /*status*/)
			{
				Emu.CallFromMainThread([]()
				{
					// Abort everything
					sys_log.notice("Aborted progress dialog");
					Emu.GracefulShutdown(false, true);
				});

				g_system_progress_canceled = true;
			};

			Emu.CallFromMainThread([dlg, text0]()
			{
				dlg->Create(text0, text0);
			});
		}

		u32 ftotal = 0;
		u32 fdone  = 0;
		u32 ptotal = 0;
		u32 pdone  = 0;
		auto text1 = text0;

		// Update progress
		while (!g_system_progress_stopping && thread_ctrl::state() != thread_state::aborting)
		{
			const auto text_new = g_progr.load();

			const u32 ftotal_new = g_progr_ftotal;
			const u32 fdone_new  = g_progr_fdone;
			const u32 ptotal_new = g_progr_ptotal;
			const u32 pdone_new  = g_progr_pdone;

			if (ftotal != ftotal_new || fdone != fdone_new || ptotal != ptotal_new || pdone != pdone_new || text_new != text1)
			{
				ftotal = ftotal_new;
				fdone  = fdone_new;
				ptotal = ptotal_new;
				pdone  = pdone_new;
				text1  = text_new;

				if (!text_new)
				{
					// Close dialog
					break;
				}

				if (show_overlay_message)
				{
					// Show a message instead
					if (g_cfg.misc.show_ppu_compilation_hint)
					{
						rsx::overlays::show_ppu_compile_notification();
					}
					thread_ctrl::wait_for(10000);
					continue;
				}

				// Compute new progress in percents
				// Assume not all programs were found if files were not compiled (as it may contain more)
				const u64 total = std::max<u64>(ptotal, 1) * std::max<u64>(ftotal, 1);
				const u64 done  = pdone * std::max<u64>(fdone, 1);
				const f32 value = static_cast<f32>(std::fmin(done * 100. / total, 100.f));

				std::string progr = "Progress:";

				if (ftotal)
					fmt::append(progr, " file %u of %u%s", fdone, ftotal, ptotal ? "," : "");
				if (ptotal)
					fmt::append(progr, " module %u of %u", pdone, ptotal);

				// Changes detected, send update
				if (native_dlg)
				{
					native_dlg->set_text(text_new);
					native_dlg->progress_bar_set_message(0, progr);
					native_dlg->progress_bar_set_value(0, std::floor(value));
				}
				else if (dlg)
				{
					Emu.CallFromMainThread([=]()
					{
						dlg->SetMsg(text_new);
						dlg->ProgressBarSetMsg(0, progr);
						dlg->ProgressBarSetValue(0, static_cast<u32>(std::floor(value)));
					});
				}
			}

			if (show_overlay_message)
			{
				// Make sure to update any pending messages. PPU compilation may freeze the image.
				rsx::overlays::refresh_message_queue();
			}

			thread_ctrl::wait_for(10000);
		}

		if (g_system_progress_stopping || thread_ctrl::state() == thread_state::aborting)
		{
			break;
		}

		if (show_overlay_message)
		{
			// Do nothing
		}
		else if (native_dlg)
		{
			native_dlg->close(false, false);
		}
		else if (dlg)
		{
			Emu.CallFromMainThread([=]()
			{
				dlg->Close(true);
			});
		}

		// Cleanup
		g_progr_fdone -= fdone;
		g_progr_pdone -= pdone;
		g_progr_ftotal -= ftotal;
		g_progr_ptotal -= ptotal;
		g_progr_ptotal.notify_all();
	}

	if (native_dlg && g_system_progress_stopping)
	{
		native_dlg->set_text("Stopping. Please wait...");
		native_dlg->refresh();
	}
}

progress_dialog_server::~progress_dialog_server()
{
	g_progr_ftotal.release(0);
	g_progr_fdone.release(0);
	g_progr_ptotal.release(0);
	g_progr_pdone.release(0);
	g_progr.release(nullptr);
}
