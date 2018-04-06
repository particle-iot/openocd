
rtos add -type freertos [ dict create \
    { target_name              } {"cortex_m" } \
    { thread_count_width       } { 4         } \
    { pointer_width            } { 4         } \
    { list_next_offset         } { 16        } \
    { list_width               } { 20        } \
    { list_elem_next_offset    } { 8         } \
    { list_elem_content_offset } { 12        } \
    { thread_stack_offset      } { 0         } \
    { thread_name_offset       } { 52        } \
] [ list \
    standard_cortex_m3 \
    standard_cortex_m4f \
    standard_cortex_m4f_fpu \
]

rtos add -type freertos [ dict create \
    { target_name              } { "hla_target" } \
    { thread_count_width       } { 4            } \
    { pointer_width            } { 4            } \
    { list_next_offset         } { 16           } \
    { list_width               } { 20           } \
    { list_elem_next_offset    } { 8            } \
    { list_elem_content_offset } { 12           } \
    { thread_stack_offset      } { 0            } \
    { thread_name_offset       } { 52           } \
] [ list \
    standard_cortex_m3 \
    standard_cortex_m4f \
    standard_cortex_m4f_fpu \
]

rtos add -type freertos [ dict create \
    { target_name              } { "nds32_v3" } \
    { thread_count_width       } { 4          } \
    { pointer_width            } { 4          } \
    { list_next_offset         } { 16         } \
    { list_width               } { 20         } \
    { list_elem_next_offset    } { 8          } \
    { list_elem_content_offset } { 12         } \
    { thread_stack_offset      } { 0          } \
    { thread_name_offset       } { 52         } \
] [ list \
    standard_nds32_n1068 \
    standard_cortex_m4f \
    standard_cortex_m4f_fpu \
]
