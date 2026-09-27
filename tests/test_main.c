/*
 * fanpro test runner.
 *
 * `make test` runs the hardware-free suite and must stay fast.
 * `make check-live` sets FANPRO_LIVE=1 to additionally run read-only checks
 * against real hardware; those are never part of the default run.
 */
#include "tinytest.h"

#include "fanpro/log.h"

#include <stdio.h>
#include <stdlib.h>

int tt_checks;
int tt_failures;
int tt_case_failures;
const char *tt_current = "(none)";

/* smc_codec */
TT_DECL(fourcc_pack);
TT_DECL(decode_flt_little_endian);
TT_DECL(encode_flt_little_endian);
TT_DECL(fpe2_is_big_endian_14_2);
TT_DECL(sp78_is_signed_7_8);
TT_DECL(fp88_and_fp4c_parse_from_fourcc);
TT_DECL(integers_are_big_endian);
TT_DECL(flag_type);
TT_DECL(unknown_type_is_refused);
TT_DECL(nan_and_inf_never_reach_the_wire);
TT_DECL(round_trip_rpm_range);
TT_DECL(null_and_zero_length_are_refused);

/* smc transport + fake backend */
TT_DECL(smc_key_info_reports_runtime_type);
TT_DECL(smc_absent_key_is_information_not_failure);
TT_DECL(smc_read_decodes_by_type);
TT_DECL(smc_write_round_trips_through_the_wire);
TT_DECL(smc_mode3_rejects_direct_write_with_0x82);
TT_DECL(smc_direct_write_succeeds_on_permissive_firmware);
TT_DECL(smc_ftst_yields_after_the_delay_not_before);
TT_DECL(smc_clearing_ftst_kills_every_manual_fan);
TT_DECL(smc_target_only_reaches_the_blades_in_manual_mode);
TT_DECL(smc_0x87_on_target_still_applies_the_value);
TT_DECL(smc_not_privileged_is_a_transport_failure);
TT_DECL(smc_lowercase_mode_key_generation);
TT_DECL(smc_key_enumeration);
TT_DECL(smc_write_to_absent_key_reports_not_found);

/* sensors */
TT_DECL(sensor_classify_by_pattern);
TT_DECL(sensor_classify_nand_wins_over_soc);
TT_DECL(sensor_classify_is_case_insensitive);
TT_DECL(sensor_add_disambiguates_duplicate_names);
TT_DECL(sensor_add_does_not_confuse_prefixes);
TT_DECL(sensor_add_reports_overflow_instead_of_truncating_silently);
TT_DECL(sensor_add_rejects_empty_names);
TT_DECL(sensor_aggregation);
TT_DECL(sensor_aggregation_of_absent_class_is_nan);
TT_DECL(sensor_aggregation_skips_invalid);

/* curve */
TT_DECL(curve_interpolates_at_and_between_points);
TT_DECL(curve_resolves_max_against_the_actual_fan);
TT_DECL(curve_holds_flat_outside_its_range);
TT_DECL(curve_rejects_unusable_input);
TT_DECL(curve_first_evaluation_is_not_slew_limited);
TT_DECL(curve_hysteresis_blocks_small_dips_only);
TT_DECL(curve_hysteresis_never_delays_a_rise);
TT_DECL(curve_slew_is_asymmetric);
TT_DECL(curve_spike_threshold_bypasses_the_upward_limit);
TT_DECL(curve_input_selection);
TT_DECL(curve_input_is_nan_when_nothing_matches);
TT_DECL(curve_eval_propagates_nan);

/* safety */
TT_DECL(safety_passes_a_reasonable_target);
TT_DECL(safety_clamps_to_the_fans_own_limits);
TT_DECL(safety_never_stops_a_fan_unless_explicitly_allowed);
TT_DECL(safety_panics_per_class_not_globally);
TT_DECL(safety_ignores_unvalidated_other_class_sensors);
TT_DECL(safety_panic_beats_a_high_target);
TT_DECL(safety_uses_thermal_pressure_as_an_independent_signal);
TT_DECL(safety_releases_when_samples_go_stale);
TT_DECL(safety_panic_outranks_staleness);
TT_DECL(safety_releases_without_a_curve_opinion);
TT_DECL(safety_releases_with_no_sensors_at_all);
TT_DECL(safety_refuses_a_fan_with_unknown_limits);
TT_DECL(safety_null_inputs_release);

/* unlock */
TT_DECL(unlock_direct_path_never_touches_ftst);
TT_DECL(unlock_falls_back_to_ftst_only_after_a_real_rejection);
TT_DECL(unlock_reports_failure_when_rejected_and_no_ftst_exists);
TT_DECL(unlock_yield_timeout_leaves_nothing_asserted);
TT_DECL(unlock_ftst_survives_releasing_one_of_two_fans);
TT_DECL(unlock_release_all_is_unconditional);
TT_DECL(unlock_recover_detects_and_clears_an_unclean_exit);
TT_DECL(unlock_recover_notices_an_orphaned_ftst);
TT_DECL(unlock_detects_an_external_client_stealing_control);
TT_DECL(unlock_detects_another_client_pinning_a_fan_we_do_not_hold);
TT_DECL(unlock_acquire_is_idempotent);
TT_DECL(unlock_rejects_an_out_of_range_fan);
TT_DECL(unlock_unprivileged_fails_without_pinning_anything);

/* review-gap regressions */
TT_DECL(gap_safety_null_sensor_set_releases);
TT_DECL(gap_acquire_undoes_a_write_it_could_not_verify);
TT_DECL(gap_release_keeps_ownership_when_the_write_fails);
TT_DECL(gap_release_all_ignores_a_transient_mode_read_failure);
TT_DECL(gap_failed_second_acquire_keeps_ftst_for_the_first_fan);
TT_DECL(gap_recover_refuses_to_run_while_we_hold_fans);
TT_DECL(gap_ftst_fallback_writes_the_mode_exactly_twice);
TT_DECL(gap_codec_sp78_encodes_negative);
TT_DECL(gap_codec_fpe2_boundary);
TT_DECL(gap_codec_flt_refuses_values_that_narrow_to_infinity);
TT_DECL(gap_codec_validates_integer_width);
TT_DECL(gap_curve_refuses_an_overlong_point_list);
TT_DECL(gap_curve_degenerate_span_behaves_as_a_step);
TT_DECL(gap_fan_zero_fans_is_success_not_failure);
TT_DECL(gap_fan_count_is_clamped_to_the_array);
TT_DECL(gap_fan_without_a_mode_key_is_readable_but_not_controllable);
TT_DECL(gap_thermal_pressure_normalises_both_scales);
TT_DECL(gap_thermal_heavy_still_trips_the_panic_threshold);

/* config */
TT_DECL(config_defaults_are_monitor_only);
TT_DECL(config_parses_a_full_file);
TT_DECL(config_rejects_a_fan_bound_to_a_missing_curve);
TT_DECL(config_rejects_unknown_keys_and_sections);
TT_DECL(config_rejects_malformed_points);
TT_DECL(config_rejects_out_of_range_values);
TT_DECL(config_rejects_a_curve_with_no_points);
TT_DECL(config_rejects_duplicate_curve_names);
TT_DECL(config_rejects_an_alert_without_a_threshold);
TT_DECL(config_failure_leaves_the_previous_config_intact);
TT_DECL(config_comments_blanks_and_whitespace);
TT_DECL(config_source_selectors);
TT_DECL(config_missing_file_yields_safe_defaults);

/* proto + peer auth */
TT_DECL(proto_valid_request_passes);
TT_DECL(proto_rejects_bad_magic_and_version);
TT_DECL(proto_rejects_unknown_verbs);
TT_DECL(proto_rejects_an_unterminated_name);
TT_DECL(proto_rejects_out_of_range_fan);
TT_DECL(proto_rejects_absurd_rpm_but_allows_nan_as_auto);
TT_DECL(proto_apply_curve_needs_a_name_and_a_fan);
TT_DECL(proto_mutating_verb_classification);
TT_DECL(peerauth_decision_table);
TT_DECL(peerauth_denies_unreadable_credentials);
TT_DECL(peerauth_finds_admin_beyond_the_first_group);

/* daemon control loop */
TT_DECL(daemon_auto_mode_never_touches_a_fan);
TT_DECL(daemon_curve_mode_drives_the_fan);
TT_DECL(daemon_deadband_suppresses_redundant_writes);
TT_DECL(daemon_panic_holds_at_max_and_does_not_flap);
TT_DECL(daemon_releases_when_sensors_disappear);
TT_DECL(daemon_latch_keeps_fans_on_auto);
TT_DECL(daemon_heartbeat_release_latches);
TT_DECL(daemon_sleep_release_does_not_latch);
TT_DECL(daemon_unbinding_a_curve_does_not_pin_the_fan);
TT_DECL(daemon_apply_curve_rejects_an_unknown_name);
TT_DECL(daemon_repeated_drift_eventually_latches);
TT_DECL(daemon_isolated_drift_does_not_latch);
TT_DECL(daemon_foreign_target_write_is_noticed);
TT_DECL(daemon_release_is_observable_to_other_threads);
TT_DECL(daemon_persistent_target_theft_latches);
TT_DECL(daemon_drift_from_another_client_releases);
TT_DECL(daemon_set_fan_command_overrides_the_curve);
TT_DECL(daemon_set_fan_auto_does_not_get_reacquired);
TT_DECL(daemon_switching_to_auto_releases_everything);
TT_DECL(effective_mode_sees_a_queued_switch_before_any_tick);
TT_DECL(effective_mode_uses_the_latest_queued_switch);
TT_DECL(effective_mode_matches_applied_mode_once_the_queue_drains);
TT_DECL(effective_mode_falls_back_to_applied_mode_behind_a_queued_reload);
TT_DECL(a_panic_threshold_guarding_no_sensor_is_reported_once);
TT_DECL(smc_temp_rejects_the_firmware_placeholder_reading);
TT_DECL(smc_temp_keeps_a_genuine_ten_degree_reading_on_other_keys);
TT_DECL(sensor_classify_puts_ta0_keys_in_the_ambient_class);
TT_DECL(daemon_command_queue_rejects_overflow);
TT_DECL(daemon_snapshot_reflects_state);
TT_DECL(daemon_tick_counter_advances_for_the_watchdog);

/* history + alerts */
TT_DECL(history_format_is_valid_jsonl);
TT_DECL(history_missing_readings_are_null_not_zero);
TT_DECL(history_format_refuses_to_overflow);
TT_DECL(history_rotates_on_a_day_boundary);
TT_DECL(alert_fires_once_on_crossing);
TT_DECL(alert_repeats_only_after_a_long_gap);
TT_DECL(alert_rearms_only_after_falling_clear);
TT_DECL(alert_ignores_unusable_values);
TT_DECL(alert_rules_evaluate_independently);
TT_DECL(history_format_truncation_never_overflows);

/* power sampling state machine */
TT_DECL(power_subscribes_once_across_many_samples);
TT_DECL(power_resubscribes_once_on_stale_subscription);
TT_DECL(power_retry_is_bounded_not_looping);
TT_DECL(power_fresh_subscription_failure_is_not_retried);
TT_DECL(power_backs_off_after_repeated_failure);
TT_DECL(power_recovers_after_backoff_expires);
TT_DECL(power_subscribe_failure_does_not_sample);
TT_DECL(power_invalidate_forces_one_resubscribe);
TT_DECL(power_invalidate_clears_active_backoff);
TT_DECL(power_null_set_is_refused);
TT_DECL(power_real_ioreport_rss_is_bounded);

int
main(void)
{
	const char *live = getenv("FANPRO_LIVE");
	const char *lvl = getenv("FANPRO_LOG_LEVEL");
	fanpro_log_level_t level = FANPRO_LOG_ERROR;

	/* Quiet by default: the suite exercises thousands of SMC writes and
	 * their INFO lines would bury the results.  Set FANPRO_LOG_LEVEL=trace
	 * when a failure needs the wire detail. */
	if (lvl != NULL)
		fanpro_log_level_from_str(lvl, &level);
	fanpro_log_set_level(level);

	printf("smc_codec\n");
	TT_RUN(fourcc_pack);
	TT_RUN(decode_flt_little_endian);
	TT_RUN(encode_flt_little_endian);
	TT_RUN(fpe2_is_big_endian_14_2);
	TT_RUN(sp78_is_signed_7_8);
	TT_RUN(fp88_and_fp4c_parse_from_fourcc);
	TT_RUN(integers_are_big_endian);
	TT_RUN(flag_type);
	TT_RUN(unknown_type_is_refused);
	TT_RUN(nan_and_inf_never_reach_the_wire);
	TT_RUN(round_trip_rpm_range);
	TT_RUN(null_and_zero_length_are_refused);

	printf("smc\n");
	TT_RUN(smc_key_info_reports_runtime_type);
	TT_RUN(smc_absent_key_is_information_not_failure);
	TT_RUN(smc_read_decodes_by_type);
	TT_RUN(smc_write_round_trips_through_the_wire);
	TT_RUN(smc_mode3_rejects_direct_write_with_0x82);
	TT_RUN(smc_direct_write_succeeds_on_permissive_firmware);
	TT_RUN(smc_ftst_yields_after_the_delay_not_before);
	TT_RUN(smc_clearing_ftst_kills_every_manual_fan);
	TT_RUN(smc_target_only_reaches_the_blades_in_manual_mode);
	TT_RUN(smc_0x87_on_target_still_applies_the_value);
	TT_RUN(smc_not_privileged_is_a_transport_failure);
	TT_RUN(smc_lowercase_mode_key_generation);
	TT_RUN(smc_key_enumeration);
	TT_RUN(smc_write_to_absent_key_reports_not_found);

	printf("sensors\n");
	TT_RUN(sensor_classify_by_pattern);
	TT_RUN(sensor_classify_nand_wins_over_soc);
	TT_RUN(sensor_classify_is_case_insensitive);
	TT_RUN(sensor_add_disambiguates_duplicate_names);
	TT_RUN(sensor_add_does_not_confuse_prefixes);
	TT_RUN(sensor_add_reports_overflow_instead_of_truncating_silently);
	TT_RUN(sensor_add_rejects_empty_names);
	TT_RUN(sensor_aggregation);
	TT_RUN(sensor_aggregation_of_absent_class_is_nan);
	TT_RUN(sensor_aggregation_skips_invalid);

	printf("curve\n");
	TT_RUN(curve_interpolates_at_and_between_points);
	TT_RUN(curve_resolves_max_against_the_actual_fan);
	TT_RUN(curve_holds_flat_outside_its_range);
	TT_RUN(curve_rejects_unusable_input);
	TT_RUN(curve_first_evaluation_is_not_slew_limited);
	TT_RUN(curve_hysteresis_blocks_small_dips_only);
	TT_RUN(curve_hysteresis_never_delays_a_rise);
	TT_RUN(curve_slew_is_asymmetric);
	TT_RUN(curve_spike_threshold_bypasses_the_upward_limit);
	TT_RUN(curve_input_selection);
	TT_RUN(curve_input_is_nan_when_nothing_matches);
	TT_RUN(curve_eval_propagates_nan);

	printf("safety\n");
	TT_RUN(safety_passes_a_reasonable_target);
	TT_RUN(safety_clamps_to_the_fans_own_limits);
	TT_RUN(safety_never_stops_a_fan_unless_explicitly_allowed);
	TT_RUN(safety_panics_per_class_not_globally);
	TT_RUN(safety_ignores_unvalidated_other_class_sensors);
	TT_RUN(safety_panic_beats_a_high_target);
	TT_RUN(safety_uses_thermal_pressure_as_an_independent_signal);
	TT_RUN(safety_releases_when_samples_go_stale);
	TT_RUN(safety_panic_outranks_staleness);
	TT_RUN(safety_releases_without_a_curve_opinion);
	TT_RUN(safety_releases_with_no_sensors_at_all);
	TT_RUN(safety_refuses_a_fan_with_unknown_limits);
	TT_RUN(safety_null_inputs_release);

	printf("unlock\n");
	TT_RUN(unlock_direct_path_never_touches_ftst);
	TT_RUN(unlock_falls_back_to_ftst_only_after_a_real_rejection);
	TT_RUN(unlock_reports_failure_when_rejected_and_no_ftst_exists);
	TT_RUN(unlock_yield_timeout_leaves_nothing_asserted);
	TT_RUN(unlock_ftst_survives_releasing_one_of_two_fans);
	TT_RUN(unlock_release_all_is_unconditional);
	TT_RUN(unlock_recover_detects_and_clears_an_unclean_exit);
	TT_RUN(unlock_recover_notices_an_orphaned_ftst);
	TT_RUN(unlock_detects_an_external_client_stealing_control);
	TT_RUN(unlock_detects_another_client_pinning_a_fan_we_do_not_hold);
	TT_RUN(unlock_acquire_is_idempotent);
	TT_RUN(unlock_rejects_an_out_of_range_fan);
	TT_RUN(unlock_unprivileged_fails_without_pinning_anything);

	printf("review gaps\n");
	TT_RUN(gap_safety_null_sensor_set_releases);
	TT_RUN(gap_acquire_undoes_a_write_it_could_not_verify);
	TT_RUN(gap_release_keeps_ownership_when_the_write_fails);
	TT_RUN(gap_release_all_ignores_a_transient_mode_read_failure);
	TT_RUN(gap_failed_second_acquire_keeps_ftst_for_the_first_fan);
	TT_RUN(gap_recover_refuses_to_run_while_we_hold_fans);
	TT_RUN(gap_ftst_fallback_writes_the_mode_exactly_twice);
	TT_RUN(gap_codec_sp78_encodes_negative);
	TT_RUN(gap_codec_fpe2_boundary);
	TT_RUN(gap_codec_flt_refuses_values_that_narrow_to_infinity);
	TT_RUN(gap_codec_validates_integer_width);
	TT_RUN(gap_curve_refuses_an_overlong_point_list);
	TT_RUN(gap_curve_degenerate_span_behaves_as_a_step);
	TT_RUN(gap_fan_zero_fans_is_success_not_failure);
	TT_RUN(gap_fan_count_is_clamped_to_the_array);
	TT_RUN(gap_fan_without_a_mode_key_is_readable_but_not_controllable);
	TT_RUN(gap_thermal_pressure_normalises_both_scales);
	TT_RUN(gap_thermal_heavy_still_trips_the_panic_threshold);

	printf("config\n");
	TT_RUN(config_defaults_are_monitor_only);
	TT_RUN(config_parses_a_full_file);
	TT_RUN(config_rejects_a_fan_bound_to_a_missing_curve);
	TT_RUN(config_rejects_unknown_keys_and_sections);
	TT_RUN(config_rejects_malformed_points);
	TT_RUN(config_rejects_out_of_range_values);
	TT_RUN(config_rejects_a_curve_with_no_points);
	TT_RUN(config_rejects_duplicate_curve_names);
	TT_RUN(config_rejects_an_alert_without_a_threshold);
	TT_RUN(config_failure_leaves_the_previous_config_intact);
	TT_RUN(config_comments_blanks_and_whitespace);
	TT_RUN(config_source_selectors);
	TT_RUN(config_missing_file_yields_safe_defaults);

	printf("proto\n");
	TT_RUN(proto_valid_request_passes);
	TT_RUN(proto_rejects_bad_magic_and_version);
	TT_RUN(proto_rejects_unknown_verbs);
	TT_RUN(proto_rejects_an_unterminated_name);
	TT_RUN(proto_rejects_out_of_range_fan);
	TT_RUN(proto_rejects_absurd_rpm_but_allows_nan_as_auto);
	TT_RUN(proto_apply_curve_needs_a_name_and_a_fan);
	TT_RUN(proto_mutating_verb_classification);
	TT_RUN(peerauth_decision_table);
	TT_RUN(peerauth_denies_unreadable_credentials);
	TT_RUN(peerauth_finds_admin_beyond_the_first_group);

	printf("daemon\n");
	TT_RUN(daemon_auto_mode_never_touches_a_fan);
	TT_RUN(daemon_curve_mode_drives_the_fan);
	TT_RUN(daemon_deadband_suppresses_redundant_writes);
	TT_RUN(daemon_panic_holds_at_max_and_does_not_flap);
	TT_RUN(daemon_releases_when_sensors_disappear);
	TT_RUN(daemon_latch_keeps_fans_on_auto);
	TT_RUN(daemon_heartbeat_release_latches);
	TT_RUN(daemon_sleep_release_does_not_latch);
	TT_RUN(daemon_unbinding_a_curve_does_not_pin_the_fan);
	TT_RUN(daemon_apply_curve_rejects_an_unknown_name);
	TT_RUN(daemon_repeated_drift_eventually_latches);
	TT_RUN(daemon_isolated_drift_does_not_latch);
	TT_RUN(daemon_foreign_target_write_is_noticed);
	TT_RUN(daemon_release_is_observable_to_other_threads);
	TT_RUN(daemon_persistent_target_theft_latches);
	TT_RUN(daemon_drift_from_another_client_releases);
	TT_RUN(daemon_set_fan_command_overrides_the_curve);
	TT_RUN(daemon_set_fan_auto_does_not_get_reacquired);
	TT_RUN(daemon_switching_to_auto_releases_everything);
	TT_RUN(daemon_command_queue_rejects_overflow);
	TT_RUN(daemon_snapshot_reflects_state);
	TT_RUN(daemon_tick_counter_advances_for_the_watchdog);
	TT_RUN(effective_mode_sees_a_queued_switch_before_any_tick);
	TT_RUN(effective_mode_uses_the_latest_queued_switch);
	TT_RUN(effective_mode_matches_applied_mode_once_the_queue_drains);
	TT_RUN(effective_mode_falls_back_to_applied_mode_behind_a_queued_reload);
	TT_RUN(a_panic_threshold_guarding_no_sensor_is_reported_once);
	TT_RUN(smc_temp_rejects_the_firmware_placeholder_reading);
	TT_RUN(smc_temp_keeps_a_genuine_ten_degree_reading_on_other_keys);
	TT_RUN(sensor_classify_puts_ta0_keys_in_the_ambient_class);

	printf("history\n");
	TT_RUN(history_format_is_valid_jsonl);
	TT_RUN(history_missing_readings_are_null_not_zero);
	TT_RUN(history_format_refuses_to_overflow);
	TT_RUN(history_rotates_on_a_day_boundary);
	TT_RUN(alert_fires_once_on_crossing);
	TT_RUN(alert_repeats_only_after_a_long_gap);
	TT_RUN(alert_rearms_only_after_falling_clear);
	TT_RUN(alert_ignores_unusable_values);
	TT_RUN(alert_rules_evaluate_independently);
	TT_RUN(history_format_truncation_never_overflows);

	printf("power\n");
	TT_RUN(power_subscribes_once_across_many_samples);
	TT_RUN(power_resubscribes_once_on_stale_subscription);
	TT_RUN(power_retry_is_bounded_not_looping);
	TT_RUN(power_fresh_subscription_failure_is_not_retried);
	TT_RUN(power_backs_off_after_repeated_failure);
	TT_RUN(power_recovers_after_backoff_expires);
	TT_RUN(power_subscribe_failure_does_not_sample);
	TT_RUN(power_invalidate_forces_one_resubscribe);
	TT_RUN(power_invalidate_clears_active_backoff);
	TT_RUN(power_null_set_is_refused);

	if (live != NULL && live[0] == '1') {
		printf("live\n");
		TT_RUN(power_real_ioreport_rss_is_bounded);
	}

	printf("\n%d checks, %d failures\n", tt_checks, tt_failures);
	return tt_failures == 0 ? 0 : 1;
}
