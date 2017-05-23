CREATE TABLE IF NOT EXISTS `pinba`.`stats` (
  `uptime` DOUBLE NOT NULL,
  `udp_poll_total` BIGINT(20) UNSIGNED NOT NULL,
  `udp_recv_total` BIGINT(20) UNSIGNED NOT NULL,
  `udp_recv_eagain` BIGINT(20) UNSIGNED NOT NULL,
  `udp_recv_bytes` BIGINT(20) UNSIGNED NOT NULL,
  `udp_recv_packets` BIGINT(20) UNSIGNED NOT NULL,
  `udp_packet_decode_err` BIGINT(20) UNSIGNED NOT NULL,
  `udp_batch_send_total` BIGINT(20) UNSIGNED NOT NULL,
  `udp_batch_send_err` BIGINT(20) UNSIGNED NOT NULL,
  `udp_ru_utime` DOUBLE NOT NULL,
  `udp_ru_stime` DOUBLE NOT NULL,
  `repacker_poll_total` BIGINT(20) UNSIGNED NOT NULL,
  `repacker_recv_total` BIGINT(20) UNSIGNED NOT NULL,
  `repacker_recv_eagain` BIGINT(20) UNSIGNED NOT NULL,
  `repacker_recv_packets` BIGINT(20) UNSIGNED NOT NULL,
  `repacker_packet_validate_err` BIGINT(20) UNSIGNED NOT NULL,
  `repacker_batch_send_total` BIGINT(20) UNSIGNED NOT NULL,
  `repacker_batch_send_by_timer` BIGINT(20) UNSIGNED NOT NULL,
  `repacker_batch_send_by_size` BIGINT(20) UNSIGNED NOT NULL,
  `repacker_ru_utime` DOUBLE NOT NULL,
  `repacker_ru_stime` DOUBLE NOT NULL,
  `coordinator_batches_received` BIGINT(20) UNSIGNED NOT NULL,
  `coordinator_batch_send_total` BIGINT(20) UNSIGNED NOT NULL,
  `coordinator_batch_send_err` BIGINT(20) UNSIGNED NOT NULL,
  `coordinator_control_requests` BIGINT(20) UNSIGNED NOT NULL,
  `coordinator_ru_utime` DOUBLE NOT NULL,
  `coordinator_ru_stime` DOUBLE NOT NULL,
  `dictionary_size` BIGINT(20) UNSIGNED NOT NULL,
  `dictionary_mem_used` BIGINT(20) UNSIGNED NOT NULL,
  `version_info` text(1024) NOT NULL,
  `build_string` text(1024) NOT NULL
) ENGINE=PINBA DEFAULT CHARSET=latin1 COMMENT='v2/stats';
