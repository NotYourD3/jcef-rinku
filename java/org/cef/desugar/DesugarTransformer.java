package org.cef.desugar;

import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;

public class DesugarTransformer {

    private static final String BACKPORT_OWNER = "org/cef/desugar/Java9Backport";

    private static final Map<String, Map<String, String>> STATIC_API_REMAPS = new HashMap<String, Map<String, String>>();
    private static final Map<String, Map<String, String>> INSTANCE_API_REMAPS = new HashMap<String, Map<String, String>>();

    static {
        Map<String, String> listRemaps = new HashMap<String, String>();
        listRemaps.put("of", "listOf");
        listRemaps.put("copyOf", "listCopyOf");
        STATIC_API_REMAPS.put("java/util/List", listRemaps);

        Map<String, String> setRemaps = new HashMap<String, String>();
        setRemaps.put("of", "setOf");
        setRemaps.put("copyOf", "setCopyOf");
        STATIC_API_REMAPS.put("java/util/Set", setRemaps);

        Map<String, String> mapRemaps = new HashMap<String, String>();
        mapRemaps.put("of", "mapOf");
        mapRemaps.put("copyOf", "mapCopyOf");
        mapRemaps.put("entry", "entry");
        STATIC_API_REMAPS.put("java/util/Map", mapRemaps);

        Map<String, String> objectsRemaps = new HashMap<String, String>();
        objectsRemaps.put("requireNonNullElse", "requireNonNullElse");
        objectsRemaps.put("requireNonNullElseGet", "requireNonNullElseGet");
        STATIC_API_REMAPS.put("java/util/Objects", objectsRemaps);

        Map<String, String> stringInstanceRemaps = new HashMap<String, String>();
        stringInstanceRemaps.put("isBlank", "stringIsBlank");
        stringInstanceRemaps.put("repeat", "stringRepeat");
        stringInstanceRemaps.put("strip", "stringStrip");
        stringInstanceRemaps.put("stripLeading", "stringStripLeading");
        stringInstanceRemaps.put("stripTrailing", "stringStripTrailing");
        INSTANCE_API_REMAPS.put("java/lang/String", stringInstanceRemaps);
    }

    public static void main(String[] args) throws IOException {
        if (args.length < 1) {
            System.err.println("Usage: DesugarTransformer <classes-directory>");
            System.exit(1);
        }
        File classesDir = new File(args[0]);
        if (!classesDir.exists() || !classesDir.isDirectory()) {
            System.err.println("Invalid directory: " + args[0]);
            System.exit(1);
        }
        int count = processDirectory(classesDir, classesDir);
        System.out.println("Desugar: transformed " + count + " class files");
    }

    private static int processDirectory(File root, File dir) throws IOException {
        int count = 0;
        File[] files = dir.listFiles();
        if (files == null) return count;
        for (File file : files) {
            if (file.isDirectory()) {
                count += processDirectory(root, file);
            } else if (file.getName().endsWith(".class")) {
                if (transformClass(file)) {
                    count++;
                }
            }
        }
        return count;
    }

    private static boolean transformClass(File classFile) throws IOException {
        byte[] originalBytes;
        FileInputStream fis = new FileInputStream(classFile);
        try {
            byte[] buf = new byte[(int) classFile.length()];
            int read = 0;
            int total = 0;
            while (total < buf.length && (read = fis.read(buf, total, buf.length - total)) != -1) {
                total += read;
            }
            originalBytes = buf;
        } finally {
            fis.close();
        }

        ClassReader cr = new ClassReader(originalBytes);
        ClassWriter cw = new ClassWriter(cr, 0);
        TransformClassVisitor cv = new TransformClassVisitor(cw);
        cr.accept(cv, 0);

        if (cv.modified) {
            byte[] transformedBytes = cw.toByteArray();
            FileOutputStream fos = new FileOutputStream(classFile);
            try {
                fos.write(transformedBytes);
            } finally {
                fos.close();
            }
            return true;
        }
        return false;
    }

    private static class TransformClassVisitor extends ClassVisitor {
        boolean modified = false;

        TransformClassVisitor(ClassVisitor cv) {
            super(Opcodes.ASM9, cv);
        }

        @Override
        public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
            MethodVisitor mv = super.visitMethod(access, name, descriptor, signature, exceptions);
            return new TransformMethodVisitor(mv, this);
        }
    }

    private static class TransformMethodVisitor extends MethodVisitor {
        private final TransformClassVisitor classVisitor;

        TransformMethodVisitor(MethodVisitor mv, TransformClassVisitor cv) {
            super(Opcodes.ASM9, mv);
            this.classVisitor = cv;
        }

        @Override
        public void visitMethodInsn(int opcode, String owner, String name, String descriptor, boolean isInterface) {
            if (opcode == Opcodes.INVOKESTATIC) {
                Map<String, String> ownerRemaps = STATIC_API_REMAPS.get(owner);
                if (ownerRemaps != null && ownerRemaps.containsKey(name)) {
                    String newName = ownerRemaps.get(name);
                    if (newName != null) {
                        classVisitor.modified = true;
                        super.visitMethodInsn(Opcodes.INVOKESTATIC, BACKPORT_OWNER, newName, descriptor, false);
                        return;
                    }
                }
            } else if (opcode == Opcodes.INVOKEVIRTUAL) {
                Map<String, String> ownerRemaps = INSTANCE_API_REMAPS.get(owner);
                if (ownerRemaps != null && ownerRemaps.containsKey(name)) {
                    String newName = ownerRemaps.get(name);
                    if (newName != null) {
                        classVisitor.modified = true;
                        String newDesc = addReceiverParameterToDescriptor(owner, descriptor);
                        super.visitMethodInsn(Opcodes.INVOKESTATIC, BACKPORT_OWNER, newName, newDesc, false);
                        return;
                    }
                }
            }
            super.visitMethodInsn(opcode, owner, name, descriptor, isInterface);
        }

        private String addReceiverParameterToDescriptor(String ownerType, String methodDescriptor) {
            int parenEnd = methodDescriptor.indexOf(')');
            String params = methodDescriptor.substring(1, parenEnd);
            String ret = methodDescriptor.substring(parenEnd);
            return "(" + "L" + ownerType + ";" + params + ret;
        }
    }
}