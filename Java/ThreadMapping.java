/**
 * This Java program is used to investigate the mapping between JVM treads and native Linux threads
 * It creates five Java threads with different priorities from MAX to MIN priority
 * The threads run a simple loop that increments a counter to keep the CPU busy
 * They also continuously print a heartbeat to show that they are alive
 * The PID of the program is printed at the start to be able to find the threads using Linux commands
 * --busy is a flag that makes the program only do calulcations instead of sleeping for 30 seconds. This was added to add more CPU load for a shorter while
 * */ 

public class ThreadMapping {
    public static void main(String[] args) throws Exception {
        long pid = ProcessHandle.current().pid();
        System.out.println("PID: " + pid);
        System.out.println("Java version: " + System.getProperty("java.version"));
        final boolean busyMode;
        final int busySeconds; // default busy duration when busy mode is enabled
        if (args.length > 0 && "--busy".equals(args[0])) {
            busyMode = true;
            int bs = 30;
            if (args.length > 1) {
                try { bs = Integer.parseInt(args[1]); } catch (NumberFormatException ignored) {}
            }
            busySeconds = bs;
            System.out.println("Running in BUSY mode for " + busySeconds + " seconds");
        } else {
            busyMode = false;
            busySeconds = 30;
        }

        String[] names = new String[] {
            "Worker-Max-Priority",
            "Worker-High-Priority",
            "Worker-Norm-Priority",
            "Worker-Low-Priority",
            "Worker-Min-Priority"
        };

        int[] priorities = new int[] {
            Thread.MAX_PRIORITY,
            8,
            Thread.NORM_PRIORITY,
            3,
            Thread.MIN_PRIORITY
        };

        final Thread[] threads = new Thread[names.length];

        for (int i = 0; i < names.length; i++) {
            final String tname = names[i];
            Thread t = new Thread(() -> {
                Thread ct = Thread.currentThread();
                System.out.printf("Thread started: name=%s javaId=%d priority=%d%n", ct.getName(), ct.getId(), ct.getPriority());
                long counter = 0L;
                if (busyMode) {
                    long end = System.currentTimeMillis() + busySeconds * 1000L;
                    while (!Thread.currentThread().isInterrupted() && System.currentTimeMillis() < end) {
                        counter++;
                        double dummy = Math.sqrt(counter % 1000);
                    }
                    System.out.printf("Busy mode finished: %s javaId=%d priority=%d counter=%d%n", ct.getName(), ct.getId(), ct.getPriority(), counter);
                } else {
                    while (!Thread.currentThread().isInterrupted()) {
                        counter++;
                        double dummy = Math.sqrt(counter % 1000); // small computation to keep thread active
                        if ((counter & 0xFFFF) == 0) {
                            System.out.printf("Heartbeat: %s javaId=%d priority=%d counter=%d%n", ct.getName(), ct.getId(), ct.getPriority(), counter);
                            try {
                                Thread.sleep(1000);
                            } catch (InterruptedException e) {
                                Thread.currentThread().interrupt();
                                break;
                            }
                        }
                    }
                }
                System.out.println(ct.getName() + " exiting.");
            }, tname);

            t.setPriority(priorities[i]);
            t.start();
            threads[i] = t;
        }

        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            System.out.println("Shutdown: interrupting worker threads...");
            for (Thread t : threads) {
                if (t != null) t.interrupt();
            }
            for (Thread t : threads) {
                if (t != null) {
                    try { t.join(2000); } catch (InterruptedException ignored) {}
                }
            }
            System.out.println("Shutdown complete.");
        }));

        // Keep main alive while workers run
        for (Thread t : threads) {
            if (t != null) t.join();
        }
    }
}
