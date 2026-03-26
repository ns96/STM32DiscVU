@echo off
echo Cleaning old files...
del *.class 2>nul
del JDeckTester.jar 2>nul

echo Compiling Java source files (Java 8 compatible)...
:: Javac with --release 8 ensures JRE 8 compatibility (JRE 9+)
javac --release 8 JMinimodem.java DurationInputStream.java JDeckTester.java
if %errorlevel% neq 0 (
    echo Compilation failed!
    pause
    exit /b %errorlevel%
)

echo Creating manifest...
echo Main-Class: JDeckTester> manifest.txt
echo.>> manifest.txt

echo Building executable JAR...
:: Attempt to find 'jar' if not in PATH
set "JAR_CMD=jar"
where %JAR_CMD% >nul 2>&1
if %errorlevel% neq 0 (
    if exist "C:\Program Files\Java\jdk-17.0.2\bin\jar.exe" (
        set "JAR_CMD=C:\Program Files\Java\jdk-17.0.2\bin\jar.exe"
    ) else if exist "C:\Program Files\Java\jdk8-portable\bin\jar.exe" (
        set "JAR_CMD=C:\Program Files\Java\jdk8-portable\bin\jar.exe"
    )
)

"%JAR_CMD%" cfm JDeckTester.jar manifest.txt *.class
if %errorlevel% neq 0 (
    echo JAR creation failed! 'jar' tool not found in PATH or standard locations.
    pause
    exit /b %errorlevel%
)

echo Cleaning up temporary files...
del *.class
del manifest.txt

echo.
echo SUCCESS! JDeckTester.jar has been created.
echo You can run it with: java -jar JDeckTester.jar
pause
