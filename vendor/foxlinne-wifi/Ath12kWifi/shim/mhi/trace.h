/* Tracing is not built; each trace point becomes an empty function. */
#ifndef LX_MHI_TRACE_H
#define LX_MHI_TRACE_H
#define trace_mhi_gen_tre(...) ((void)0)
#define trace_mhi_intvec_states(...) ((void)0)
#define trace_mhi_tryset_pm_state(...) ((void)0)
#define trace_mhi_process_ev_ring(...) ((void)0)
#define trace_mhi_ctrl_event(...) ((void)0)
#define trace_mhi_data_event(...) ((void)0)
#define trace_mhi_pm_state(...) ((void)0)
#define trace_mhi_ring_ev_db(...) ((void)0)
#define trace_mhi_ring_chan_db(...) ((void)0)
#define trace_mhi_tryset_state(...) ((void)0)
#define trace_mhi_pm_st_transition(...) ((void)0)
#define trace_mhi_update_channel_state(...) ((void)0)
#define trace_mhi_ring_er_db(...) ((void)0)
#define trace_mhi_channel_command_start(...) ((void)0)
#define trace_mhi_channel_command_end(...) ((void)0)
#define TPS(x) (x)
#endif
