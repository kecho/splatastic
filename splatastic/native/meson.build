py.extension_module(
    'native',
    [
        'ModuleMain.cpp',
        'utils/ByteBuffer.cpp',
        'utils/ClTokenizer.cpp',
        'tasks/TaskSystem.cpp',
        'tasks/ThreadWorker.cpp',
        'files/FileSystem.cpp',
        'files/InternalFileSystem.cpp',
        'files/Utils.cpp',
        'scene/SceneDb.cpp',
        'scene/PlyParser.cpp'
    ],
    # cpp_args: ['-fno-exceptions', '-fno-rtti', '-D__STDC_VERSION__=0' ],
    install: true,
    subdir: 'splatastic'
)
