# 统一编译选项。所有目标都应调用 mfweb_apply_common_options(<target>)。
function(mfweb_apply_common_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4             # 高警告等级（目标：零警告）
            /permissive-    # 严格标准符合性
            /Zc:__cplusplus # 让 __cplusplus 反映真实标准版本
            /Zc:preprocessor
            /utf-8          # 源码与执行字符集均为 UTF-8
            /EHsc
            /bigobj         # 编译期反射会产生大量模板实例
        )
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS
        )
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
