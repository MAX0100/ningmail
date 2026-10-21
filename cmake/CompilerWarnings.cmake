# Warnings for first-party targets only (see spec 8.2; third-party libraries excluded).

function(ningmail_apply_warnings target)
    target_compile_options(${target}
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
    )
endfunction()
