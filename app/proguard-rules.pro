# Add project specific ProGuard rules here.
# Keep JNI entry points so the native side can resolve them
-keep class com.livevideo.media.** { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
